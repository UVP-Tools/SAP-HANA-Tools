/******************************************************************************
 * Client-facing interface for the Xenbus driver.  In other words, the
 * interface between the Xenbus and the device-specific code, be it the
 * frontend or the backend of that driver.
 *
 * Copyright (C) 2005 XenSource Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#if defined(CONFIG_XEN) || defined(MODULE)
#include <linux/slab.h>
#include <xen/evtchn.h>
#include <xen/gnttab.h>
#include <xen/driver_util.h>
#else
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <asm/xen/hypervisor.h>
#include <xen/interface/xen.h>
#include <xen/interface/event_channel.h>
#include <xen/events.h>
#include <xen/grant_table.h>
#endif
#include <xen/xenbus.h>

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

const char *xenbus_strstate(enum xenbus_state state)
{
	static const char *const name[] = {
		[ XenbusStateUnknown      ] = "Unknown",
		[ XenbusStateInitialising ] = "Initialising",
		[ XenbusStateInitWait     ] = "InitWait",
		[ XenbusStateInitialised  ] = "Initialised",
		[ XenbusStateConnected    ] = "Connected",
		[ XenbusStateClosing      ] = "Closing",
		[ XenbusStateClosed	  ] = "Closed",
		[ XenbusStateReconfiguring ] = "Reconfiguring",
		[ XenbusStateReconfigured ] = "Reconfigured",
	};
	return (state < ARRAY_SIZE(name)) ? name[state] : "INVALID";
}
EXPORT_SYMBOL_GPL(xenbus_strstate);

/**
 * xenbus_watch_path - register a watch
 * @dev: xenbus device
 * @path: path to watch
 * @watch: watch to register
 * @callback: callback to register
 *
 * Register a @watch on the given path, using the given xenbus_watch structure
 * for storage, and the given @callback function as the callback.  Return 0 on
 * success, or -errno on error.  On success, the given @path will be saved as
 * @watch->node, and remains the caller's to free.  On error, @watch->node will
 * be NULL, the device will switch to %XenbusStateClosing, and the error will
 * be saved in the store.
 */
int xenbus_watch_path(struct xenbus_device *dev, const char *path,
		      struct xenbus_watch *watch,
		      void (*callback)(struct xenbus_watch *,
				       const char **, unsigned int))
{
	int err;

	watch->node = path;
	watch->callback = callback;

	err = register_xenbus_watch(watch);

	if (err) {
		watch->node = NULL;
		watch->callback = NULL;
		xenbus_dev_fatal(dev, err, "adding watch on %s", path);
	}

	return err;
}
EXPORT_SYMBOL_GPL(xenbus_watch_path);


#if defined(CONFIG_XEN) || defined(MODULE)
int xenbus_watch_path2(struct xenbus_device *dev, const char *path,
		       const char *path2, struct xenbus_watch *watch,
		       void (*callback)(struct xenbus_watch *,
					const char **, unsigned int))
{
	int err;
	char *state = kasprintf(GFP_NOIO | __GFP_HIGH, "%s/%s", path, path2);
	if (!state) {
		xenbus_dev_fatal(dev, -ENOMEM, "allocating path for watch");
		return -ENOMEM;
	}
	err = xenbus_watch_path(dev, state, watch, callback);

	if (err)
		kfree(state);
	return err;
}
EXPORT_SYMBOL_GPL(xenbus_watch_path2);
#else
/**
 * xenbus_watch_pathfmt - register a watch on a sprintf-formatted path
 * @dev: xenbus device
 * @watch: watch to register
 * @callback: callback to register
 * @pathfmt: format of path to watch
 *
 * Register a watch on the given @path, using the given xenbus_watch
 * structure for storage, and the given @callback function as the callback.
 * Return 0 on success, or -errno on error.  On success, the watched path
 * (@path/@path2) will be saved as @watch->node, and becomes the caller's to
 * kfree().  On error, watch->node will be NULL, so the caller has nothing to
 * free, the device will switch to %XenbusStateClosing, and the error will be
 * saved in the store.
 */
int xenbus_watch_pathfmt(struct xenbus_device *dev,
			 struct xenbus_watch *watch,
			 void (*callback)(struct xenbus_watch *,
					const char **, unsigned int),
			 const char *pathfmt, ...)
{
	int err;
	va_list ap;
	char *path;

	va_start(ap, pathfmt);
	path = kvasprintf(GFP_NOIO | __GFP_HIGH, pathfmt, ap);
	va_end(ap);

	if (!path) {
		xenbus_dev_fatal(dev, -ENOMEM, "allocating path for watch");
		return -ENOMEM;
	}
	err = xenbus_watch_path(dev, path, watch, callback);

	if (err)
		kfree(path);
	return err;
}
EXPORT_SYMBOL_GPL(xenbus_watch_pathfmt);
#endif


/**
 * xenbus_switch_state
 * @dev: xenbus device
 * @state: new state
 *
 * Advertise in the store a change of the given driver to the given new_state.
 * Return 0 on success, or -errno on error.  On error, the device will switch
 * to XenbusStateClosing, and the error will be saved in the store.
 */
int xenbus_switch_state(struct xenbus_device *dev, enum xenbus_state state)
{
	/* We check whether the state is currently set to the given value, and
	   if not, then the state is set.  We don't want to unconditionally
	   write the given state, because we don't want to fire watches
	   unnecessarily.  Furthermore, if the node has gone, we don't write
	   to it, as the device will be tearing down, and we don't want to
	   resurrect that directory.

	   Note that, because of this cached value of our state, this function
	   will not work inside a Xenstore transaction (something it was
	   trying to in the past) because dev->state would not get reset if
	   the transaction was aborted.

	 */

	int current_state;
	int err;

	if (state == dev->state)
		return 0;

	err = xenbus_scanf(XBT_NIL, dev->nodename, "state", "%d",
			   &current_state);
	if (err != 1)
		return 0;

	err = xenbus_printf(XBT_NIL, dev->nodename, "state", "%d", state);
	if (err) {
		if (state != XenbusStateClosing) /* Avoid looping */
			xenbus_dev_fatal(dev, err, "writing new state");
		return err;
	}

	dev->state = state;

	return 0;
}
EXPORT_SYMBOL_GPL(xenbus_switch_state);

int xenbus_frontend_closed(struct xenbus_device *dev)
{
	xenbus_switch_state(dev, XenbusStateClosed);
	complete(&dev->down);
	return 0;
}
EXPORT_SYMBOL_GPL(xenbus_frontend_closed);

/**
 * Return the path to the error node for the given device, or NULL on failure.
 * If the value returned is non-NULL, then it is the caller's to kfree.
 */
static char *error_path(struct xenbus_device *dev)
{
	return kasprintf(GFP_KERNEL, "error/%s", dev->nodename);
}


static void _dev_error(struct xenbus_device *dev, int err,
			const char *fmt, va_list ap)
{
	int ret;
	unsigned int len;
	char *printf_buffer = NULL, *path_buffer = NULL;

#define PRINTF_BUFFER_SIZE 4096
	printf_buffer = kmalloc(PRINTF_BUFFER_SIZE, GFP_KERNEL);
	if (printf_buffer == NULL)
		goto fail;

	len = sprintf(printf_buffer, "%i ", -err);
	ret = vsnprintf(printf_buffer+len, PRINTF_BUFFER_SIZE-len, fmt, ap);

	BUG_ON(len + ret > PRINTF_BUFFER_SIZE-1);

	dev_err(&dev->dev, "%s\n", printf_buffer);

	path_buffer = error_path(dev);

	if (path_buffer == NULL) {
		dev_err(&dev->dev,
		        "xenbus: failed to write error node for %s (%s)\n",
		        dev->nodename, printf_buffer);
		goto fail;
	}

	if (xenbus_write(XBT_NIL, path_buffer, "error", printf_buffer) != 0) {
		dev_err(&dev->dev,
		        "xenbus: failed to write error node for %s (%s)\n",
		        dev->nodename, printf_buffer);
		goto fail;
	}

fail:
	kfree(printf_buffer);
	kfree(path_buffer);
}


/**
 * xenbus_dev_error
 * @dev: xenbus device
 * @err: error to report
 * @fmt: error message format
 *
 * Report the given negative errno into the store, along with the given
 * formatted message.
 */
void xenbus_dev_error(struct xenbus_device *dev, int err, const char *fmt,
		      ...)
{
	va_list ap;

	va_start(ap, fmt);
	_dev_error(dev, err, fmt, ap);
	va_end(ap);
}
EXPORT_SYMBOL_GPL(xenbus_dev_error);


/**
 * xenbus_dev_fatal
 * @dev: xenbus device
 * @err: error to report
 * @fmt: error message format
 *
 * Equivalent to xenbus_dev_error(dev, err, fmt, args), followed by
 * xenbus_switch_state(dev, XenbusStateClosing) to schedule an orderly
 * closedown of this driver and its peer.
 */
void xenbus_dev_fatal(struct xenbus_device *dev, int err, const char *fmt,
		      ...)
{
	va_list ap;

	va_start(ap, fmt);
	_dev_error(dev, err, fmt, ap);
	va_end(ap);

	xenbus_switch_state(dev, XenbusStateClosing);
}
EXPORT_SYMBOL_GPL(xenbus_dev_fatal);


/**
 * xenbus_grant_ring
 * @dev: xenbus device
 * @ring_mfn: mfn of ring to grant
 *
 * Grant access to the given @ring_mfn to the peer of the given device.  Return
 * 0 on success, or -errno on error.  On error, the device will switch to
 * XenbusStateClosing, and the error will be saved in the store.
 */
int xenbus_grant_ring(struct xenbus_device *dev, unsigned long ring_mfn)
{
	int err = gnttab_grant_foreign_access(dev->otherend_id, ring_mfn, 0);
	if (err < 0)
		xenbus_dev_fatal(dev, err, "granting access to ring page");
	return err;
}
EXPORT_SYMBOL_GPL(xenbus_grant_ring);


/**
 * Allocate an event channel for the given xenbus_device, assigning the newly
 * created local port to *port.  Return 0 on success, or -errno on error.  On
 * error, the device will switch to XenbusStateClosing, and the error will be
 * saved in the store.
 */
int xenbus_alloc_evtchn(struct xenbus_device *dev, int *port)
{
	struct evtchn_alloc_unbound alloc_unbound;
	int err;

	alloc_unbound.dom        = DOMID_SELF;
	alloc_unbound.remote_dom = dev->otherend_id;

	err = HYPERVISOR_event_channel_op(EVTCHNOP_alloc_unbound,
					  &alloc_unbound);
	if (err)
		xenbus_dev_fatal(dev, err, "allocating event channel");
	else
		*port = alloc_unbound.port;

	return err;
}
EXPORT_SYMBOL_GPL(xenbus_alloc_evtchn);


/**
 * Free an existing event channel. Returns 0 on success or -errno on error.
 */
int xenbus_free_evtchn(struct xenbus_device *dev, int port)
{
	struct evtchn_close close;
	int err;

	close.port = port;

	err = HYPERVISOR_event_channel_op(EVTCHNOP_close, &close);
	if (err)
		xenbus_dev_error(dev, err, "freeing event channel %d", port);

	return err;
}
EXPORT_SYMBOL_GPL(xenbus_free_evtchn);


/**
 * xenbus_read_driver_state
 * @path: path for driver
 *
 * Return the state of the driver rooted at the given store path, or
 * XenbusStateUnknown if no state can be read.
 */
enum xenbus_state xenbus_read_driver_state(const char *path)
{
	enum xenbus_state result;
	int err = xenbus_gather(XBT_NIL, path, "state", "%d", &result, NULL);
	if (err)
		result = XenbusStateUnknown;

	return result;
}
EXPORT_SYMBOL_GPL(xenbus_read_driver_state);

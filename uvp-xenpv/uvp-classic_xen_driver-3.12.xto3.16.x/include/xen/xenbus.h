/******************************************************************************
 * xenbus.h
 *
 * Talks to Xen Store to figure out what devices we have.
 *
 * Copyright (C) 2005 Rusty Russell, IBM Corporation
 * Copyright (C) 2005 XenSource Ltd.
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

#ifndef _XEN_XENBUS_H
#define _XEN_XENBUS_H

#include <linux/device.h>
#include <linux/notifier.h>
#include <linux/mutex.h>
#include <linux/export.h>
#include <linux/completion.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/version.h>
#include <xen/interface/xen.h>
#include <xen/interface/grant_table.h>
#include <xen/interface/io/xenbus.h>
#include <xen/interface/io/xs_wire.h>

/* Register callback to watch this node. */
struct xenbus_watch
{
	struct list_head list;

	/* Path being watched. */
	const char *node;

	/* Callback (executed in a process context with no locks held). */
	void (*callback)(struct xenbus_watch *,
			 const char **vec, unsigned int len);

#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
	/* See XBWF_ definitions below. */
	unsigned long flags;
#endif
};

#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
/*
 * Execute callback in its own kthread. Useful if the callback is long
 * running or heavily serialised, to avoid taking out the main xenwatch thread
 * for a long period of time (or even unwittingly causing a deadlock).
 */
#define XBWF_new_thread	1
#endif

/* A xenbus device. */
struct xenbus_device {
	const char *devicetype;
	const char *nodename;
	const char *otherend;
	int otherend_id;
	struct xenbus_watch otherend_watch;
	struct device dev;
	enum xenbus_state state;
	struct completion down;
#if !defined(CONFIG_XEN) && !defined(HAVE_XEN_PLATFORM_COMPAT_H)
	struct work_struct work;
#endif
};

static inline struct xenbus_device *to_xenbus_device(struct device *dev)
{
	return container_of(dev, struct xenbus_device, dev);
}

struct xenbus_device_id
{
	/* .../device/<device_type>/<identifier> */
	char devicetype[32]; 	/* General class of device. */
};

/* A xenbus driver. */
struct xenbus_driver {
	const struct xenbus_device_id *ids;
	int (*probe)(struct xenbus_device *dev,
		     const struct xenbus_device_id *id);
	void (*otherend_changed)(struct xenbus_device *dev,
				 enum xenbus_state backend_state);
	int (*remove)(struct xenbus_device *dev);
	int (*suspend)(struct xenbus_device *dev);
#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
	int (*suspend_cancel)(struct xenbus_device *dev);
#endif
	int (*resume)(struct xenbus_device *dev);
	int (*uevent)(struct xenbus_device *, struct kobj_uevent_env *);
	struct device_driver driver;
	int (*read_otherend_details)(struct xenbus_device *dev);
	int (*is_ready)(struct xenbus_device *dev);
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,10)
# define XENBUS_DRIVER_SET_OWNER(mod) .driver.owner = mod,
#else
# define XENBUS_DRIVER_SET_OWNER(mod)
#endif

#define DEFINE_XENBUS_DRIVER(var, drvname, methods...)		\
struct xenbus_driver var ## _driver = {				\
	.driver.name = drvname + 0 ?: var ## _ids->devicetype,	\
	.driver.mod_name = KBUILD_MODNAME,			\
	XENBUS_DRIVER_SET_OWNER(THIS_MODULE)			\
	.ids = var ## _ids, ## methods				\
}

static inline struct xenbus_driver *to_xenbus_driver(struct device_driver *drv)
{
	return container_of(drv, struct xenbus_driver, driver);
}

int __must_check xenbus_register_frontend(struct xenbus_driver *drv);
int __must_check xenbus_register_backend(struct xenbus_driver *drv);
void xenbus_unregister_driver(struct xenbus_driver *drv);

struct xenbus_transaction
{
	u32 id;
};

/* Nil transaction ID. */
#define XBT_NIL ((struct xenbus_transaction) { 0 })

char **xenbus_directory(struct xenbus_transaction t,
			const char *dir, const char *node, unsigned int *num);
void *xenbus_read(struct xenbus_transaction t,
		  const char *dir, const char *node, unsigned int *len);
int xenbus_write(struct xenbus_transaction t,
		 const char *dir, const char *node, const char *string);
int xenbus_mkdir(struct xenbus_transaction t,
		 const char *dir, const char *node);
int xenbus_exists(struct xenbus_transaction t,
		  const char *dir, const char *node);
int xenbus_rm(struct xenbus_transaction t, const char *dir, const char *node);
int xenbus_transaction_start(struct xenbus_transaction *t);
int xenbus_transaction_end(struct xenbus_transaction t, int abort);

/* Single read and scanf: returns -errno or num scanned if > 0. */
__scanf(4, 5)
int xenbus_scanf(struct xenbus_transaction t,
		 const char *dir, const char *node, const char *fmt, ...);

/* Single printf and write: returns -errno or 0. */
__printf(4, 5)
int xenbus_printf(struct xenbus_transaction t,
		  const char *dir, const char *node, const char *fmt, ...);

/* Generic read function: NULL-terminated triples of name,
 * sprintf-style type string, and pointer. Returns 0 or errno.*/
int xenbus_gather(struct xenbus_transaction t, const char *dir, ...);

/* notifer routines for when the xenstore comes up */
int register_xenstore_notifier(struct notifier_block *nb);
void unregister_xenstore_notifier(struct notifier_block *nb);

int register_xenbus_watch(struct xenbus_watch *watch);
void unregister_xenbus_watch(struct xenbus_watch *watch);
void xs_suspend(void);
void xs_resume(void);
void xs_suspend_cancel(void);

/* Used by xenbus_dev to borrow kernel's store connection. */
void *xenbus_dev_request_and_reply(struct xsd_sockmsg *msg);

struct work_struct;
void xenbus_probe(struct work_struct *);

/* Prepare for domain suspend: then resume or cancel the suspend. */
void xenbus_suspend(void);
void xenbus_resume(void);
void xenbus_suspend_cancel(void);

#define XENBUS_IS_ERR_READ(str) ({			\
	if (!IS_ERR(str) && strlen(str) == 0) {		\
		kfree(str);				\
		str = ERR_PTR(-ERANGE);			\
	}						\
	IS_ERR(str);					\
})

#define XENBUS_EXIST_ERR(err) ((err) == -ENOENT || (err) == -ERANGE)


/**
 * Register a watch on the given path, using the given xenbus_watch structure
 * for storage, and the given callback function as the callback.  Return 0 on
 * success, or -errno on error.  On success, the given path will be saved as
 * watch->node, and remains the caller's to free.  On error, watch->node will
 * be NULL, the device will switch to XenbusStateClosing, and the error will
 * be saved in the store.
 */
int xenbus_watch_path(struct xenbus_device *dev, const char *path,
		      struct xenbus_watch *watch,
		      void (*callback)(struct xenbus_watch *,
				       const char **, unsigned int));


#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
/**
 * Register a watch on the given path/path2, using the given xenbus_watch
 * structure for storage, and the given callback function as the callback.
 * Return 0 on success, or -errno on error.  On success, the watched path
 * (path/path2) will be saved as watch->node, and becomes the caller's to
 * kfree().  On error, watch->node will be NULL, so the caller has nothing to
 * free, the device will switch to XenbusStateClosing, and the error will be
 * saved in the store.
 */
int xenbus_watch_path2(struct xenbus_device *dev, const char *path,
		       const char *path2, struct xenbus_watch *watch,
		       void (*callback)(struct xenbus_watch *,
					const char **, unsigned int));
#else
__printf(4, 5)
int xenbus_watch_pathfmt(struct xenbus_device *dev, struct xenbus_watch *watch,
			 void (*callback)(struct xenbus_watch *,
					  const char **, unsigned int),
			 const char *pathfmt, ...);
#endif

/**
 * Advertise in the store a change of the given driver to the given new_state.
 * Return 0 on success, or -errno on error.  On error, the device will switch
 * to XenbusStateClosing, and the error will be saved in the store.
 */
int xenbus_switch_state(struct xenbus_device *dev, enum xenbus_state new_state);

/**
 * Grant access to the given ring_mfn to the peer of the given device.  Return
 * 0 on success, or -errno on error.  On error, the device will switch to
 * XenbusStateClosing, and the error will be saved in the store.
 */
int xenbus_grant_ring(struct xenbus_device *dev, unsigned long ring_mfn);

int xenbus_multi_grant_ring(struct xenbus_device *, unsigned int nr,
			    struct page *[], grant_ref_t []);

/**
 * Map a page of memory into this domain from another domain's grant table.
 * xenbus_map_ring_valloc allocates a page of virtual address space, maps the
 * page to that address, and sets *vaddr to that address.
 * Returns 0 on success, and GNTST_* (see xen/include/interface/grant_table.h)
 * or -ENOMEM on error. If an error is returned, device will switch to
 * XenbusStateClosing and the error message will be saved in XenStore.
 */
#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
struct vm_struct *xenbus_map_ring_valloc(struct xenbus_device *dev,
					 const grant_ref_t refs[],
					 unsigned int nr_refs);
#else
int xenbus_map_ring_valloc(struct xenbus_device *dev,
			   grant_ref_t gnt_ref, void **vaddr);
#endif
int xenbus_map_ring(struct xenbus_device *dev, grant_ref_t gnt_ref,
			   grant_handle_t *handle, void *vaddr);

/**
 * Unmap a page of memory in this domain that was imported from another domain
 * and free the virtual address space.
 * Returns 0 on success and returns GNTST_* on error
 * (see xen/include/interface/grant_table.h).
 */
#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
int xenbus_unmap_ring_vfree(struct xenbus_device *dev, struct vm_struct *);
#else
int xenbus_unmap_ring_vfree(struct xenbus_device *dev, void *vaddr);
#endif
int xenbus_unmap_ring(struct xenbus_device *dev,
		      grant_handle_t handle, void *vaddr);

/**
 * Allocate an event channel for the given xenbus_device, assigning the newly
 * created local port to *port.  Return 0 on success, or -errno on error.  On
 * error, the device will switch to XenbusStateClosing, and the error will be
 * saved in the store.
 */
int xenbus_alloc_evtchn(struct xenbus_device *dev, int *port);


/**
 * Free an existing event channel. Returns 0 on success or -errno on error.
 */
int xenbus_free_evtchn(struct xenbus_device *dev, int port);


/**
 * Return the state of the driver rooted at the given store path, or
 * XenbusStateUnknown if no state can be read.
 */
enum xenbus_state xenbus_read_driver_state(const char *path);


/***
 * Report the given negative errno into the store, along with the given
 * formatted message.
 */
void xenbus_dev_error(struct xenbus_device *dev, int err, const char *fmt,
		      ...) __printf(3, 4);

/***
 * Equivalent to xenbus_dev_error(dev, err, fmt, args), followed by
 * xenbus_switch_state(dev, NULL, XenbusStateClosing) to schedule an orderly
 * closedown of this driver and its peer.
 */
void xenbus_dev_fatal(struct xenbus_device *dev, int err, const char *fmt,
		      ...) __printf(3, 4);

#if defined(CONFIG_XEN) || defined(HAVE_XEN_PLATFORM_COMPAT_H)
int xenbus_dev_init(void);
#endif

const char *xenbus_strstate(enum xenbus_state state);
int xenbus_dev_is_online(struct xenbus_device *dev);
int xenbus_frontend_closed(struct xenbus_device *dev);

int xenbus_for_each_backend(void *arg, int (*fn)(struct device *, void *));
int xenbus_for_each_frontend(void *arg, int (*fn)(struct device *, void *));

#endif /* _XEN_XENBUS_H */

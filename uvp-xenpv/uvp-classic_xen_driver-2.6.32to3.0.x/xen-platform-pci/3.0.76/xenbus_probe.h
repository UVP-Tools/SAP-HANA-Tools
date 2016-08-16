/******************************************************************************
 * xenbus_probe.h
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

#ifndef _XENBUS_PROBE_H
#define _XENBUS_PROBE_H

#ifndef BUS_ID_SIZE
#define XEN_BUS_ID_SIZE			20
#else
#define XEN_BUS_ID_SIZE			BUS_ID_SIZE
#endif

#ifdef CONFIG_PARAVIRT_XEN
#define is_running_on_xen() xen_domain()
#define is_initial_xendomain() xen_initial_domain()
#elif LINUX_VERSION_CODE < KERNEL_VERSION(2,6,26)
#define dev_name(dev) ((dev)->bus_id)
#endif

#if defined(CONFIG_XEN_BACKEND) || defined(CONFIG_XEN_BACKEND_MODULE)
extern void xenbus_backend_suspend(int (*fn)(struct device *, void *));
extern void xenbus_backend_resume(int (*fn)(struct device *, void *));
extern void xenbus_backend_probe_and_watch(void);
extern void xenbus_backend_bus_register(void);
extern void xenbus_backend_device_register(void);
#else
static inline void xenbus_backend_suspend(int (*fn)(struct device *, void *)) {}
static inline void xenbus_backend_resume(int (*fn)(struct device *, void *)) {}
static inline void xenbus_backend_probe_and_watch(void) {}
static inline void xenbus_backend_bus_register(void) {}
static inline void xenbus_backend_device_register(void) {}
#endif

struct xen_bus_type
{
	char *root;
	int error;
	unsigned int levels;
	int (*get_bus_id)(char bus_id[XEN_BUS_ID_SIZE], const char *nodename);
	int (*probe)(struct xen_bus_type *bus, const char *type,
		     const char *dir);
#if !defined(CONFIG_XEN) && !defined(HAVE_XEN_PLATFORM_COMPAT_H)
	void (*otherend_changed)(struct xenbus_watch *watch, const char **vec,
				 unsigned int len);
#else
	struct device dev;
#endif
	struct bus_type bus;
};

extern int xenbus_match(struct device *_dev, struct device_driver *_drv);
extern int xenbus_dev_probe(struct device *_dev);
extern int xenbus_dev_remove(struct device *_dev);
extern int xenbus_register_driver_common(struct xenbus_driver *drv,
					 struct xen_bus_type *bus);
extern int xenbus_probe_node(struct xen_bus_type *bus,
			     const char *type,
			     const char *nodename);
extern int xenbus_probe_devices(struct xen_bus_type *bus);

extern void xenbus_dev_changed(const char *node, struct xen_bus_type *bus);

extern void xenbus_dev_shutdown(struct device *_dev);

extern int xenbus_dev_suspend(struct device *dev);
extern int xenbus_dev_resume(struct device *dev);
extern int xenbus_dev_cancel(struct device *dev);

extern void xenbus_otherend_changed(struct xenbus_watch *watch,
				    const char **vec, unsigned int len,
				    int ignore_on_shutdown);

extern int xenbus_read_otherend_details(struct xenbus_device *xendev,
					char *id_node, char *path_node);

#endif

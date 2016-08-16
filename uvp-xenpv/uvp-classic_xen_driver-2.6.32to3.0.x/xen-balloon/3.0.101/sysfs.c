/******************************************************************************
 * balloon/sysfs.c
 *
 * Xen balloon driver - sysfs interfaces.
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

#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sysdev.h>
#include <linux/module.h>
#include "common.h"

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

#define BALLOON_CLASS_NAME "xen_memory"

#define BALLOON_SHOW(name, format, args...)			\
	static ssize_t show_##name(struct sys_device *dev,	\
				   struct sysdev_attribute *attr, \
				   char *buf)			\
	{							\
		return sprintf(buf, format, ##args);		\
	}							\
	static SYSDEV_ATTR(name, S_IRUGO, show_##name, NULL)

BALLOON_SHOW(current_kb, "%lu\n", PAGES2KB(bs.current_pages));
BALLOON_SHOW(min_kb, "%lu\n", PAGES2KB(balloon_minimum_target()));
BALLOON_SHOW(max_kb, "%lu\n", PAGES2KB(num_physpages));
BALLOON_SHOW(low_kb, "%lu\n", PAGES2KB(bs.balloon_low));
BALLOON_SHOW(high_kb, "%lu\n", PAGES2KB(bs.balloon_high));
BALLOON_SHOW(driver_kb, "%lu\n", PAGES2KB(bs.driver_pages));

static ssize_t show_target_kb(struct sys_device *dev,
			      struct sysdev_attribute *attr, char *buf)
{
	return sprintf(buf, "%lu\n", PAGES2KB(bs.target_pages));
}

static ssize_t store_target_kb(struct sys_device *dev,
			       struct sysdev_attribute *attr,
			       const char *buf, size_t count)
{
	char *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	
	if (count <= 1)
		return -EBADMSG; /* runt */
	
	target_bytes = simple_strtoull(buf, &endchar, 0) << 10;
	balloon_set_new_target(target_bytes >> PAGE_SHIFT);
	
	return count;
}

static SYSDEV_ATTR(target_kb, S_IRUGO | S_IWUSR,
		   show_target_kb, store_target_kb);

static ssize_t show_target(struct sys_device *dev,
			   struct sysdev_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n",
		       (unsigned long long)balloon_stats.target_pages
		       << PAGE_SHIFT);
}

static ssize_t store_target(struct sys_device *dev,
			    struct sysdev_attribute *attr,
			    const char *buf,
			    size_t count)
{
	char *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count <= 1)
		return -EBADMSG; /* runt */

	target_bytes = memparse(buf, &endchar);
	balloon_set_new_target(target_bytes >> PAGE_SHIFT);

	return count;
}

static SYSDEV_ATTR(target, S_IRUGO | S_IWUSR,
		   show_target, store_target);

static struct sysdev_attribute *balloon_attrs[] = {
	&attr_target_kb,
	&attr_target,
};

static struct attribute *balloon_info_attrs[] = {
	&attr_current_kb.attr,
	&attr_min_kb.attr,
	&attr_max_kb.attr,
	&attr_low_kb.attr,
	&attr_high_kb.attr,
	&attr_driver_kb.attr,
	NULL
};

static const struct attribute_group balloon_info_group = {
	.name = "info",
	.attrs = balloon_info_attrs,
};

static struct sysdev_class balloon_sysdev_class = {
	.name = BALLOON_CLASS_NAME,
};

static struct sys_device balloon_sysdev;

static int __init register_balloon(struct sys_device *sysdev)
{
	int i, error;

	error = sysdev_class_register(&balloon_sysdev_class);
	if (error)
		return error;

	sysdev->id = 0;
	sysdev->cls = &balloon_sysdev_class;

	error = sysdev_register(sysdev);
	if (error) {
		sysdev_class_unregister(&balloon_sysdev_class);
		return error;
	}

	for (i = 0; i < ARRAY_SIZE(balloon_attrs); i++) {
		error = sysdev_create_file(sysdev, balloon_attrs[i]);
		if (error)
			goto fail;
	}

	error = sysfs_create_group(&sysdev->kobj, &balloon_info_group);
	if (error)
		goto fail;
	
	return 0;

 fail:
	while (--i >= 0)
		sysdev_remove_file(sysdev, balloon_attrs[i]);
	sysdev_unregister(sysdev);
	sysdev_class_unregister(&balloon_sysdev_class);
	return error;
}

static __exit void unregister_balloon(struct sys_device *sysdev)
{
	int i;

	sysfs_remove_group(&sysdev->kobj, &balloon_info_group);
	for (i = 0; i < ARRAY_SIZE(balloon_attrs); i++)
		sysdev_remove_file(sysdev, balloon_attrs[i]);
	sysdev_unregister(sysdev);
	sysdev_class_unregister(&balloon_sysdev_class);
}

int __init balloon_sysfs_init(void)
{
	return register_balloon(&balloon_sysdev);
}

void __exit balloon_sysfs_exit(void)
{
	unregister_balloon(&balloon_sysdev);
}

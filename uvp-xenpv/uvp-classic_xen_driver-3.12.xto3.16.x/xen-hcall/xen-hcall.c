/*
 * A proc driver giving user-space access to call the hypercall connection to
 * Xen hypervisor.
 *
 * Copyright 2016, Huawei Tech. Co., Ltd.
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


#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/proc_fs.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <asm/uaccess.h>
#include <xen/interface/io/xs_wire.h>
#include <xen/xenbus.h>
#include <xen/interface/xen.h>
#include <asm/hypervisor.h>

#if (defined PROC_FS_DECLARE)
#include "linux/internal.h"
#include "linux/hypercall.h"
#else
#include <asm/hypercall.h>
#endif


MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Xen DomU Hypercall Filesystem");

/*Definable hypercall number*/
#define __HYPERVISOR_hvm_private_op 60
/*Definable hypercall operation cmd*/
#define HVMOP_get_utc_time 0

#define BUFFER_SIZE 100

struct xen_hvm_hosttime_param  {
   uint64_t utc_time;    /* nsec */
};

#if (defined PROC_XEN_VERSION)
static struct proc_dir_entry *proc_xen;
static struct proc_dir_entry *proc_xen_version;
#endif

static struct proc_dir_entry *proc_entry;

static ssize_t hcall_read(struct file *filp,
			       char __user *ubuf,
			       size_t len, loff_t *ppos)
{
	int ret = 0;
	struct xen_hvm_hosttime_param a;
	char kbuf[BUFFER_SIZE] = {0};
	
	a.utc_time = 0;		
	
	ret = _hypercall2(unsigned long, hvm_private_op, 0, &a);

	if(ret < 0)
	{
		printk( KERN_INFO "Xen-hcall: DmoU call hypercall fail! Error Code : %d.\n", ret );
		return ret;
	}
	
	snprintf(kbuf, BUFFER_SIZE, "%llu", a.utc_time);
	ret = copy_to_user(ubuf, kbuf, BUFFER_SIZE);

	return ret;
}

static const struct file_operations hcall_file_ops = {
	.read = hcall_read,
	//.write = hcall_write,
	//.open = hcall_open,
	//.release = hcall_release,
	//.poll = hcall_poll,
};

static int hcall_procfs_init(void)
{
    int ret = 0;

    proc_entry = proc_create( "xen_hcall", S_IRUSR, NULL, &hcall_file_ops);
    if ( proc_entry == NULL )
    {
        ret = -ENOMEM;
        printk( KERN_INFO "Xen-hcall: Couldn't create DmoU hypercall user-space entry!\n");
    }
    else
    {
        printk( KERN_INFO "Xen_hcall: DomU hypercall user-space has been created.\n" );
    }

#if (defined PROC_XEN_VERSION)
    struct file *filp = NULL;
    struct inode *inode = NULL;
    struct proc_dir_entry *pe = NULL;
    /* get parent proc_dir_entry */
    filp = filp_open("/proc/xen", O_RDONLY, 0644);
    if (NULL == filp)
    {
        proc_xen_version = proc_create( "xen/version", S_IRUSR, NULL, &hcall_file_ops);
        if (NULL == proc_xen_version)
        {
            ret = -ENOMEM;
            printk( KERN_INFO "Xen-hcall: Couldn't create xen_version!\n");
        }
        return ret;
    }
    inode = filp->f_path.dentry->d_inode;
    if (NULL == inode)
    {
        filp_close(filp, NULL);
        filp = NULL;
        return -ENOMEM;
    }
    pe = PDE(inode);
    if (NULL == pe)
    {
        filp_close(filp, NULL);
        filp = NULL;
        return -ENOMEM;
    }
    if (S_ISDIR(pe->mode) && !pe->proc_iops)
    {
        printk(KERN_INFO "The file %s is permanently empty directory!\n", pe->name);
        filp_close(filp, NULL);
        filp = NULL;
        remove_proc_entry("xen", NULL);
        proc_xen = proc_mkdir("xen", NULL);
        if (NULL == proc_xen)
        {
            printk(KERN_INFO "Create dir /proc/xen error!\n");
            return -ENOMEM;
        }
        proc_xen_version = proc_create("version", S_IRUSR, proc_xen, &hcall_file_ops);
        if (NULL == proc_xen_version)
        {
            printk(KERN_INFO "Xen-hcall: Couldn't create xen_version!\n");
            return -ENOMEM;
        }
    }
    else
    {
        proc_xen_version = proc_create("version", S_IRUSR, pe, &hcall_file_ops);
        if (NULL == proc_xen_version)
        {
            ret = -ENOMEM;
            printk(KERN_INFO "Xen-hcall: Couldn't create xen_version!\n");
        }
        filp_close(filp, NULL);
        filp = NULL;
    }
#endif

    /*if VRM wirte control/vrm_flag value*/
#if defined(VRM)
    xenbus_write(XBT_NIL, "control/uvp", "vrm_flag", "true");
#endif

    return ret;
}
module_init(hcall_procfs_init);

static void hcall_procfs_exit(void)
{
    remove_proc_entry( "xen_hcall", NULL );

#if (defined PROC_XEN_VERSION)
    if (proc_xen_version)
    {
        remove_proc_entry( "xen/version", NULL );
    }
#endif

    printk( KERN_INFO "xen_hcall: hypercall user-space has been deleted.\n" );
}
module_exit(hcall_procfs_exit);

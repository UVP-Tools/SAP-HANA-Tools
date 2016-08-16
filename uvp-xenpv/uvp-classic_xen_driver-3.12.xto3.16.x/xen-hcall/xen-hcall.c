/*****************************************************************************
*                Copyright 2013, Huawei Tech. Co., Ltd.            
*                           ALL RIGHTS RESERVED                           
* FileName: xen_hcall.c
* Author: wujinwen 00206189                                                       
* Date: 2013-01-08                                                          
* Description: A proc driver giving user-space access to call the hypercall connection
* to xen hypervisor.                                  
* Others:                                                                   
*****************************************************************************/

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
#include <asm/hypercall.h>

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
    printk( KERN_INFO "xen_hcall: hypercall user-space has been deleted.\n" );
}
module_exit(hcall_procfs_exit);

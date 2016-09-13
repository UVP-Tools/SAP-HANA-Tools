/******************************************************************************
 * platform-pci.c
 *
 * Xen platform PCI device driver
 * Copyright (c) 2005, Intel Corporation.
 * Copyright (c) 2007, XenSource Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <asm/hypervisor.h>
#include <asm/pgtable.h>
#include <xen/interface/memory.h>
#include <xen/interface/hvm/params.h>
#include <xen/features.h>
#include <xen/evtchn.h>
#ifdef __ia64__
#include <asm/xen/xencomm.h>
#endif

#include "platform-pci.h"
#include <xen/xenbus.h>

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

#define DRV_NAME    "xen-platform-pci"
#define DRV_VERSION "0.10"
#define DRV_RELDATE "03/03/2005"

static int max_hypercall_stub_pages, nr_hypercall_stub_pages;
char *hypercall_stubs;
EXPORT_SYMBOL(hypercall_stubs);

MODULE_AUTHOR("ssmith@xensource.com");
MODULE_DESCRIPTION("Xen platform PCI device");
MODULE_LICENSE("GPL");

struct pci_dev *xen_platform_pdev;

static unsigned long shared_info_frame;
static uint64_t callback_via;

/* driver should write xenstore flag to tell xen which feature supported */
void  write_feature_flag()
{
	int rc;
	char str[32] = {0};

	(void)snprintf(str, sizeof(str), "%d", SUPPORTED_FEATURE);
	rc = xenbus_write(XBT_NIL, "control/uvp", "feature_flag", str);
	if (rc) {
		printk(KERN_INFO DRV_NAME "write  control/uvp/feature_flag failed \n");
	}
}

/*2012-7-9 write monitor-service-flag is true*/
void  write_monitor_service_flag()
{
	int rc;

	rc = xenbus_write(XBT_NIL, "control/uvp", "monitor-service-flag", "true");
	if (rc) {
		printk(KERN_INFO DRV_NAME "write  control/uvp/monitor-service-flag failed \n");
	}
}

/*DTS2014042508949. driver write this flag when the xenpci resume succeed.*/
void  write_driver_resume_flag()
{
	int rc = 0;

	rc = xenbus_write(XBT_NIL, "control/uvp", "driver-resume-flag", "1");
	if (rc) {
		printk(KERN_INFO DRV_NAME "write  control/uvp/driver-resume-flag failed \n");
	}
}

static int __devinit init_xen_info(void)
{
	struct xen_add_to_physmap xatp;
	extern void *shared_info_area;

#ifdef __ia64__
	xencomm_initialize();
#endif

	setup_xen_features();

	shared_info_frame = alloc_xen_mmio(PAGE_SIZE) >> PAGE_SHIFT;
	xatp.domid = DOMID_SELF;
	xatp.idx = 0;
	xatp.space = XENMAPSPACE_shared_info;
	xatp.gpfn = shared_info_frame;
	if (HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp))
		BUG();

	shared_info_area =
		ioremap(shared_info_frame << PAGE_SHIFT, PAGE_SIZE);
	if (shared_info_area == NULL)
		panic("can't map shared info\n");

	return 0;
}

static unsigned long platform_mmio;
static unsigned long platform_mmio_alloc;
static unsigned long platform_mmiolen;

unsigned long alloc_xen_mmio(unsigned long len)
{
	unsigned long addr;

	addr = platform_mmio + platform_mmio_alloc;
	platform_mmio_alloc += len;
	BUG_ON(platform_mmio_alloc > platform_mmiolen);

	return addr;
}

#ifndef __ia64__

static uint32_t xen_cpuid_base(void)
{
	uint32_t base, eax, ebx, ecx, edx;
	char signature[13];

	for (base = 0x40000000; base < 0x40010000; base += 0x100) {
		cpuid(base, &eax, &ebx, &ecx, &edx);
		*(uint32_t*)(signature + 0) = ebx;
		*(uint32_t*)(signature + 4) = ecx;
		*(uint32_t*)(signature + 8) = edx;
		signature[12] = 0;

		if (!strcmp("XenVMMXenVMM", signature) && ((eax - base) >= 2))
			return base;
	}

	return 0;
}

static int init_hypercall_stubs(void)
{
	uint32_t eax, ebx, ecx, edx, pages, msr, i, base;

	base = xen_cpuid_base();
	if (base == 0) {
		printk(KERN_WARNING
		       "Detected Xen platform device but not Xen VMM?\n");
		return -EINVAL;
	}

	cpuid(base + 1, &eax, &ebx, &ecx, &edx);

	printk(KERN_INFO "Xen version %d.%d.\n", eax >> 16, eax & 0xffff);

	/*
	 * Find largest supported number of hypercall pages.
	 * We'll create as many as possible up to this number.
	 */
	cpuid(base + 2, &pages, &msr, &ecx, &edx);

	/*
	 * Use __vmalloc() because vmalloc_exec() is not an exported symbol.
	 * PAGE_KERNEL_EXEC also is not exported, hence we use PAGE_KERNEL.
	 * hypercall_stubs = vmalloc_exec(pages * PAGE_SIZE);
	 */
	while (pages > 0) {
		hypercall_stubs = __vmalloc(
			pages * PAGE_SIZE,
			GFP_KERNEL | __GFP_HIGHMEM,
			__pgprot(__PAGE_KERNEL & ~_PAGE_NX));
		if (hypercall_stubs != NULL)
			break;
		pages--; /* vmalloc failed: try one fewer pages */
	}

	if (hypercall_stubs == NULL)
		return -ENOMEM;

	for (i = 0; i < pages; i++) {
		unsigned long pfn;
		pfn = vmalloc_to_pfn((char *)hypercall_stubs + i*PAGE_SIZE);
		wrmsrl(msr, ((u64)pfn << PAGE_SHIFT) + i);
	}

	nr_hypercall_stub_pages = pages;
	max_hypercall_stub_pages = pages;

	printk(KERN_INFO "Hypercall area is %u pages.\n", pages);

	return 0;
}

static void resume_hypercall_stubs(void)
{
	uint32_t base, ecx, edx, pages, msr, i;

	base = xen_cpuid_base();
	BUG_ON(base == 0);

	cpuid(base + 2, &pages, &msr, &ecx, &edx);

	if (pages > max_hypercall_stub_pages)
		pages = max_hypercall_stub_pages;

	for (i = 0; i < pages; i++) {
		unsigned long pfn;
		pfn = vmalloc_to_pfn((char *)hypercall_stubs + i*PAGE_SIZE);
		wrmsrl(msr, ((u64)pfn << PAGE_SHIFT) + i);
	}

	nr_hypercall_stub_pages = pages;
}

#else /* __ia64__ */

#define init_hypercall_stubs()		(0)
#define resume_hypercall_stubs()	((void)0)

#endif

static uint64_t get_callback_via(struct pci_dev *pdev)
{
	u8 pin;
	int irq;

#ifdef __ia64__
	for (irq = 0; irq < 16; irq++) {
		if (isa_irq_to_vector(irq) == pdev->irq)
			return irq; /* ISA IRQ */
	}
#else /* !__ia64__ */
	irq = pdev->irq;
	if (irq < 16)
		return irq; /* ISA IRQ */
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,16)
	pin = pdev->pin;
#else
	pci_read_config_byte(pdev, PCI_INTERRUPT_PIN, &pin);
#endif

	/* We don't know the GSI. Specify the PCI INTx line instead. */
	return (((uint64_t)0x01 << 56) | /* PCI INTx identifier */
		((uint64_t)pci_domain_nr(pdev->bus) << 32) |
		((uint64_t)pdev->bus->number << 16) |
		((uint64_t)(pdev->devfn & 0xff) << 8) |
		((uint64_t)(pin - 1) & 3));
}

static int set_callback_via(uint64_t via)
{
	struct xen_hvm_param a;

	a.domid = DOMID_SELF;
	a.index = HVM_PARAM_CALLBACK_IRQ;
	a.value = via;
	return HYPERVISOR_hvm_op(HVMOP_set_param, &a);
}

int xen_irq_init(struct pci_dev *pdev);
int xenbus_init(void);
int xen_reboot_init(void);
int xen_panic_handler_init(void);
int gnttab_init(void);
void xen_unplug_emulated_devices(void);

static int __devinit platform_pci_init(struct pci_dev *pdev,
				       const struct pci_device_id *ent)
{
	int i, ret;
	long ioaddr, iolen;
	long mmio_addr, mmio_len;

	xen_unplug_emulated_devices();

	if (xen_platform_pdev)
		return -EBUSY;
	xen_platform_pdev = pdev;

	i = pci_enable_device(pdev);
	if (i)
		return i;

	ioaddr = pci_resource_start(pdev, 0);
	iolen = pci_resource_len(pdev, 0);

	mmio_addr = pci_resource_start(pdev, 1);
	mmio_len = pci_resource_len(pdev, 1);

	callback_via = get_callback_via(pdev);

	if (mmio_addr == 0 || ioaddr == 0 || callback_via == 0) {
		printk(KERN_WARNING DRV_NAME ":no resources found\n");
		return -ENOENT;
	}

	if (request_mem_region(mmio_addr, mmio_len, DRV_NAME) == NULL) {
		printk(KERN_ERR ":MEM I/O resource 0x%lx @ 0x%lx busy\n",
		       mmio_addr, mmio_len);
		return -EBUSY;
	}

	if (request_region(ioaddr, iolen, DRV_NAME) == NULL) {
		printk(KERN_ERR DRV_NAME ":I/O resource 0x%lx @ 0x%lx busy\n",
		       iolen, ioaddr);
		release_mem_region(mmio_addr, mmio_len);
		return -EBUSY;
	}

	platform_mmio = mmio_addr;
	platform_mmiolen = mmio_len;

	ret = init_hypercall_stubs();
	if (ret < 0)
		goto out;

	if ((ret = init_xen_info()))
		goto out;

	if ((ret = gnttab_init()))
		goto out;

	if ((ret = xen_irq_init(pdev)))
		goto out;

	if ((ret = set_callback_via(callback_via)))
		goto out;

	if ((ret = xenbus_init()))
		goto out;

	if ((ret = xen_reboot_init()))
		goto out;
	/* write flag to xenstore when xenbus is available */
	write_feature_flag();

 out:
	if (ret) {
		release_mem_region(mmio_addr, mmio_len);
		release_region(ioaddr, iolen);
	}

	return ret;
}

#define XEN_PLATFORM_VENDOR_ID 0x5853
#define XEN_PLATFORM_DEVICE_ID 0x0001
static struct pci_device_id platform_pci_tbl[] __devinitdata = {
	{XEN_PLATFORM_VENDOR_ID, XEN_PLATFORM_DEVICE_ID,
	 PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	/* Continue to recognise the old ID for now */
	{0xfffd, 0x0101, PCI_ANY_ID, PCI_ANY_ID, 0, 0, 0},
	{0,}
};

MODULE_DEVICE_TABLE(pci, platform_pci_tbl);

static struct pci_driver platform_driver = {
	name:     DRV_NAME,
	probe:    platform_pci_init,
	id_table: platform_pci_tbl,
};

static int pci_device_registered;

void platform_pci_resume(void)
{
	struct xen_add_to_physmap xatp;

	resume_hypercall_stubs();

	xatp.domid = DOMID_SELF;
	xatp.idx = 0;
	xatp.space = XENMAPSPACE_shared_info;
	xatp.gpfn = shared_info_frame;
	if (HYPERVISOR_memory_op(XENMEM_add_to_physmap, &xatp))
		BUG();

	if (set_callback_via(callback_via))
		printk("platform_pci_resume failure!\n");
}

static int __init platform_pci_module_init(void)
{
	int rc;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,10)
	rc = pci_module_init(&platform_driver);
#else
	rc = pci_register_driver(&platform_driver);
#endif
	if (rc) {
		printk(KERN_INFO DRV_NAME
		       ": No platform pci device model found\n");
		return rc;
	}

	pci_device_registered = 1;
	return 0;
}

module_init(platform_pci_module_init);

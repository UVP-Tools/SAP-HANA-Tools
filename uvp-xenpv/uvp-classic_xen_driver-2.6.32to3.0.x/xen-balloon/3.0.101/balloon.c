/******************************************************************************
 * balloon.c
 *
 * Xen balloon driver - enables returning/claiming memory to/from Xen.
 *
 * Copyright (c) 2003, B Dragovic
 * Copyright (c) 2003-2004, M Williamson, K Fraser
 * Copyright (c) 2005 Dan M. Smith, IBM Corporation
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/bootmem.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <linux/string.h>
#include <xen/xen_proc.h>
#include <asm/hypervisor.h>
#include <xen/balloon.h>
#include <xen/interface/memory.h>
#include <asm/maddr.h>
#include <asm/page.h>
#include <asm/pgalloc.h>
#include <asm/pgtable.h>
#include <asm/uaccess.h>
#include <asm/tlb.h>
#include <xen/xenbus.h>
#include "common.h"

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

#ifdef CONFIG_PROC_FS
static struct proc_dir_entry *balloon_pde;
#endif

static DEFINE_MUTEX(balloon_mutex);

/*
 * Protects atomic reservation decrease/increase against concurrent increases.
 * Also protects non-atomic updates of current_pages and driver_pages, and
 * balloon lists.
 */
DEFINE_SPINLOCK(balloon_lock);

#ifndef MODULE
#include <linux/pagevec.h>
static struct pagevec free_pagevec;
#endif

struct balloon_stats balloon_stats;

/* We increase/decrease in batches which fit in a page */
static unsigned long frame_list[PAGE_SIZE / sizeof(unsigned long)];

#ifdef CONFIG_HIGHMEM
#define inc_totalhigh_pages() (totalhigh_pages++)
#define dec_totalhigh_pages() (totalhigh_pages--)
#else
#define inc_totalhigh_pages() ((void)0)
#define dec_totalhigh_pages() ((void)0)
#endif

#ifndef CONFIG_XEN
/*
 * In HVM guests accounting here uses the Xen visible values, but the kernel
 * determined totalram_pages value shouldn't get altered. Since totalram_pages
 * includes neither the kernel static image nor any memory allocated prior to
 * or from the bootmem allocator, we have to synchronize the two values.
 */
static unsigned long __read_mostly totalram_bias;
#else
#define totalram_bias 0
#endif

/* List of ballooned pages, threaded through the mem_map array. */
static LIST_HEAD(ballooned_pages);

/* Main work function, always executed in process context. */
static void balloon_process(struct work_struct *unused);
static DECLARE_WORK(balloon_worker, balloon_process);
static struct timer_list balloon_timer;

/* When ballooning out (allocating memory to return to Xen) we don't really 
   want the kernel to try too hard since that can trigger the oom killer. */
#define GFP_BALLOON \
	(__GFP_HIGHMEM|__GFP_NOWARN|__GFP_NORETRY|GFP_ATOMIC)

#define PAGE_TO_LIST(p) (&(p)->lru)
#define LIST_TO_PAGE(l) list_entry((l), struct page, lru)
#define UNLIST_PAGE(p)				\
	do {					\
		list_del(PAGE_TO_LIST(p));	\
		PAGE_TO_LIST(p)->next = NULL;	\
		PAGE_TO_LIST(p)->prev = NULL;	\
	} while(0)

#define IPRINTK(fmt, args...) pr_info("xen_mem: " fmt, ##args)
#define WPRINTK(fmt, args...) pr_warning("xen_mem: " fmt, ##args)

/* balloon_append: add the given page to the balloon. */
static void balloon_append(struct page *page, int account)
{
	unsigned long pfn;

	/* Lowmem is re-populated first, so highmem pages go at list tail. */
	if (PageHighMem(page)) {
		list_add_tail(PAGE_TO_LIST(page), &ballooned_pages);
		bs.balloon_high++;
		if (account)
			dec_totalhigh_pages();
	} else {
		list_add(PAGE_TO_LIST(page), &ballooned_pages);
		bs.balloon_low++;
	}

	pfn = page_to_pfn(page);
	if (account) {
		SetPageReserved(page);
		set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
		page_zone(page)->present_pages--;
	} else {
		BUG_ON(!PageReserved(page));
		WARN_ON_ONCE(phys_to_machine_mapping_valid(pfn));
	}
}

/* balloon_retrieve: rescue a page from the balloon, if it is not empty. */
static struct page *balloon_retrieve(int *was_empty)
{
	struct page *page;
	struct zone *zone;

	if (list_empty(&ballooned_pages))
		return NULL;

	page = LIST_TO_PAGE(ballooned_pages.next);
	UNLIST_PAGE(page);
	BUG_ON(!PageReserved(page));

	if (PageHighMem(page)) {
		bs.balloon_high--;
		inc_totalhigh_pages();
	}
	else
		bs.balloon_low--;
	zone = page_zone(page);
	*was_empty |= !populated_zone(zone);
	zone->present_pages++;

	return page;
}

static struct page *balloon_first_page(void)
{
	if (list_empty(&ballooned_pages))
		return NULL;
	return LIST_TO_PAGE(ballooned_pages.next);
}

static struct page *balloon_next_page(struct page *page)
{
	struct list_head *next = PAGE_TO_LIST(page)->next;
	if (next == &ballooned_pages)
		return NULL;
	return LIST_TO_PAGE(next);
}

static inline void balloon_free_page(struct page *page)
{
#ifndef MODULE
	if (put_page_testzero(page) && !pagevec_add(&free_pagevec, page)) {
		__pagevec_free(&free_pagevec);
		pagevec_reinit(&free_pagevec);
	}
#else
	/* pagevec interface is not being exported. */
	__free_page(page);
#endif
}

static inline void balloon_free_and_unlock(unsigned long flags)
{
#ifndef MODULE
	if (pagevec_count(&free_pagevec)) {
		__pagevec_free(&free_pagevec);
		pagevec_reinit(&free_pagevec);
	}
#endif
	balloon_unlock(flags);
}

static void balloon_alarm(unsigned long unused)
{
	schedule_work(&balloon_worker);
}
static DEFINE_TIMER(balloon_timer, balloon_alarm, 0, 0);

static unsigned long current_target(void)
{
	unsigned long target = bs.target_pages;
	if (target > (bs.current_pages + bs.balloon_low + bs.balloon_high))
		target = bs.current_pages + bs.balloon_low + bs.balloon_high;
	return target;
}

unsigned long balloon_minimum_target(void)
{
#ifndef CONFIG_XEN
#define max_pfn num_physpages
#endif
	unsigned long min_pages, curr_pages = current_target();

#define MB2PAGES(mb) ((mb) << (20 - PAGE_SHIFT))
	/* Simple continuous piecewiese linear function:
	 *  max MiB -> min MiB	gradient
	 *       0	   0
	 *      16	  16
	 *      32	  24
	 *     128	  72	(1/2)
	 *     512 	 168	(1/4)
	 *    2048	 360	(1/8)
	 *    8192	 552	(1/32)
	 *   32768	1320
	 *  131072	4392
	 */
	if (max_pfn < MB2PAGES(128))
		min_pages = MB2PAGES(8) + (max_pfn >> 1);
	else if (max_pfn < MB2PAGES(512))
		min_pages = MB2PAGES(40) + (max_pfn >> 2);
	else if (max_pfn < MB2PAGES(2048))
		min_pages = MB2PAGES(104) + (max_pfn >> 3);
	else
		min_pages = MB2PAGES(296) + (max_pfn >> 5);
#undef MB2PAGES

	/* Don't enforce growth */
	return min(min_pages, curr_pages);
#ifndef CONFIG_XEN
#undef max_pfn
#endif
}

static int increase_reservation(unsigned long nr_pages)
{
	unsigned long  pfn, i, flags;
	struct page   *page;
	long           rc;
	int            need_zonelists_rebuild = 0;
	struct xen_memory_reservation reservation = {
		.domid = DOMID_SELF
	};

	if (nr_pages > ARRAY_SIZE(frame_list))
		nr_pages = ARRAY_SIZE(frame_list);

	balloon_lock(flags);

	page = balloon_first_page();
	for (i = 0; i < nr_pages; i++) {
		BUG_ON(page == NULL);
		frame_list[i] = page_to_pfn(page);;
		page = balloon_next_page(page);
	}

	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents = nr_pages;
	rc = HYPERVISOR_memory_op(XENMEM_populate_physmap, &reservation);
	if (rc < 0)
		goto out;

	for (i = 0; i < rc; i++) {
		page = balloon_retrieve(&need_zonelists_rebuild);
		BUG_ON(page == NULL);

		pfn = page_to_pfn(page);
		BUG_ON(!xen_feature(XENFEAT_auto_translated_physmap) &&
		       phys_to_machine_mapping_valid(pfn));

		set_phys_to_machine(pfn, frame_list[i]);

#ifdef CONFIG_XEN
		/* Link back into the page tables if not highmem. */
		if (pfn < max_low_pfn) {
			int ret;
			ret = HYPERVISOR_update_va_mapping(
				(unsigned long)__va(pfn << PAGE_SHIFT),
				pfn_pte_ma(frame_list[i], PAGE_KERNEL),
				0);
			BUG_ON(ret);
		}
#endif

		/* Relinquish the page back to the allocator. */
		ClearPageReserved(page);
		init_page_count(page);
		balloon_free_page(page);
	}

	bs.current_pages += rc;
	totalram_pages = bs.current_pages - totalram_bias;

 out:
	balloon_free_and_unlock(flags);

#ifndef MODULE
	setup_per_zone_wmarks();
	if (rc > 0)
		kswapd_run(0);
	if (need_zonelists_rebuild)
		build_all_zonelists(NULL);
	else
		vm_total_pages = nr_free_pagecache_pages();
#endif

	return rc < 0 ? rc : rc != nr_pages;
}

static int decrease_reservation(unsigned long nr_pages)
{
	unsigned long  pfn, i, flags;
	struct page   *page;
	void          *v;
	int            need_sleep = 0;
	int ret;
	struct xen_memory_reservation reservation = {
		.domid = DOMID_SELF
	};

	if (nr_pages > ARRAY_SIZE(frame_list))
		nr_pages = ARRAY_SIZE(frame_list);

	for (i = 0; i < nr_pages; i++) {
		if ((page = alloc_page(GFP_BALLOON)) == NULL) {
			nr_pages = i;
			need_sleep = 1;
			break;
		}

		pfn = page_to_pfn(page);
		frame_list[i] = pfn_to_mfn(pfn);

		if (!PageHighMem(page)) {
			v = phys_to_virt(pfn << PAGE_SHIFT);
			xen_scrub_pages(v, 1);
#ifdef CONFIG_XEN
			ret = HYPERVISOR_update_va_mapping(
				(unsigned long)v, __pte_ma(0), 0);
			BUG_ON(ret);
#endif
		}
#ifdef CONFIG_XEN_SCRUB_PAGES
		else {
			v = kmap(page);
			xen_scrub_pages(v, 1);
			kunmap(page);
		}
#endif
	}

#ifdef CONFIG_XEN
	/* Ensure that ballooned highmem pages don't have kmaps. */
	kmap_flush_unused();
	flush_tlb_all();
#endif

	balloon_lock(flags);

	/* No more mappings: invalidate P2M and add to balloon. */
	for (i = 0; i < nr_pages; i++) {
		pfn = mfn_to_pfn(frame_list[i]);
		balloon_append(pfn_to_page(pfn), 1);
	}

	set_xen_guest_handle(reservation.extent_start, frame_list);
	reservation.nr_extents   = nr_pages;
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
	BUG_ON(ret != nr_pages);

	bs.current_pages -= nr_pages;
	totalram_pages = bs.current_pages - totalram_bias;

	balloon_unlock(flags);

	return need_sleep;
}

/*
 * We avoid multiple worker processes conflicting via the balloon mutex.
 * We may of course race updates of the target counts (which are protected
 * by the balloon lock), or with changes to the Xen hard limit, but we will
 * recover from these in time.
 */
static int flag;
static void balloon_process(struct work_struct *unused)
{
	int need_sleep = 0;
	long credit;
	char buffer[16];

	mutex_lock(&balloon_mutex);
   	if(totalram_pages > (bs.current_pages - totalram_bias))
    {
        IPRINTK("totalram_pages=%lu, current_pages=%lu, totalram_bias=%lu\n", totalram_pages*4, bs.current_pages*4, totalram_bias*4);
        bs.current_pages = totalram_pages + totalram_bias;
        IPRINTK("totalram_pages=%lu, current_pages=%lu\n", totalram_pages*4, bs.current_pages*4);
    }

    if(current_target() == bs.current_pages)
	{
		mutex_unlock(&balloon_mutex);
		return;
	}
	xenbus_write(XBT_NIL, "control/uvp", "Balloon_flag", "1");

	do {
		credit = current_target() - bs.current_pages;
		if (credit > 0)
			need_sleep = (increase_reservation(credit) != 0);
		if (credit < 0)
			need_sleep = (decrease_reservation(-credit) != 0);

#ifndef CONFIG_PREEMPT
		if (need_resched())
			schedule();
#endif
	} while ((credit != 0) && !need_sleep);

	/* Schedule more work if there is some still to be done. */
	if (current_target() != bs.current_pages || (flag==1))
	{
		sprintf(buffer,"%lu",bs.current_pages<<(PAGE_SHIFT-10));
		xenbus_write(XBT_NIL, "memory", "target", buffer);
	}
	xenbus_write(XBT_NIL, "control/uvp", "Balloon_flag", "0");

	mutex_unlock(&balloon_mutex);
}

extern unsigned int migrate_state;
/* Resets the Xen limit, sets new target, and kicks off processing. */
void balloon_set_new_target(unsigned long target)
{
	/* No need for lock. Not read-modify-write updates. */
	bs.target_pages = max(target, balloon_minimum_target());
	flag = bs.target_pages== target ? 0:1;
	if(!migrate_state)
	{
		schedule_work(&balloon_worker);
	}
	else
	{
	    IPRINTK("totalram_pages=%lu k, current_pages=%lu k migrate_state:%d\n", totalram_pages*4, bs.current_pages*4, migrate_state);
	}
}

static struct xenbus_watch target_watch =
{
	.node = "memory/target"
};

/* React to a change in the target key */
static void watch_target(struct xenbus_watch *watch,
			 const char **vec, unsigned int len)
{
	unsigned long long new_target;
	int err;

	err = xenbus_scanf(XBT_NIL, "memory", "target", "%llu", &new_target);
	if (err != 1) {
		/* This is ok (for domain0 at least) - so just return */
		return;
	}

	/* The given memory/target value is in KiB, so it needs converting to
	 * pages. PAGE_SHIFT converts bytes to pages, hence PAGE_SHIFT - 10.
	 */
	balloon_set_new_target(new_target >> (PAGE_SHIFT - 10));
}

static int balloon_init_watcher(struct notifier_block *notifier,
				unsigned long event,
				void *data)
{
	int err;

	err = register_xenbus_watch(&target_watch);
	if (err)
		pr_err("Failed to set balloon watcher\n");

	return NOTIFY_DONE;
}

#ifdef CONFIG_PROC_FS
static int balloon_write(struct file *file, const char __user *buffer,
			 unsigned long count, void *data)
{
	char memstring[64], *endchar;
	unsigned long long target_bytes;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (count <= 1)
		return -EBADMSG; /* runt */
	if (count > sizeof(memstring))
		return -EFBIG;   /* too long */

	if (copy_from_user(memstring, buffer, count))
		return -EFAULT;
	memstring[sizeof(memstring)-1] = '\0';

	target_bytes = memparse(memstring, &endchar);
	balloon_set_new_target(target_bytes >> PAGE_SHIFT);

	return count;
}

static int balloon_read(char *page, char **start, off_t off,
			int count, int *eof, void *data)
{
	int len;

	len = sprintf(
		page,
		"Current allocation: %8lu kB\n"
		"Requested target:   %8lu kB\n"
		"Minimum target:     %8lu kB\n"
		"Maximum target:     %8lu kB\n"
		"Low-mem balloon:    %8lu kB\n"
		"High-mem balloon:   %8lu kB\n"
		"Driver pages:       %8lu kB\n",
		PAGES2KB(bs.current_pages), PAGES2KB(bs.target_pages), 
		PAGES2KB(balloon_minimum_target()), PAGES2KB(num_physpages),
		PAGES2KB(bs.balloon_low), PAGES2KB(bs.balloon_high),
		PAGES2KB(bs.driver_pages));


	*eof = 1;
	return len;
}
#endif

static struct notifier_block xenstore_notifier;

static int __init balloon_init(void)
{
#if !defined(CONFIG_XEN)
# ifndef XENMEM_get_pod_target
#  define XENMEM_get_pod_target 17
	typedef struct xen_pod_target {
		uint64_t target_pages;
		uint64_t tot_pages;
		uint64_t pod_cache_pages;
		uint64_t pod_entries;
		domid_t domid;
	} xen_pod_target_t;
# endif
	xen_pod_target_t pod_target = { .domid = DOMID_SELF };
	int rc;
#elif defined(CONFIG_X86)
	unsigned long pfn;
	struct page *page;
#endif

	if (!is_running_on_xen())
		return -ENODEV;

	IPRINTK("Initialising balloon driver.\n");

#ifdef CONFIG_XEN
	pagevec_init(&free_pagevec, true);
	bs.current_pages = min(xen_start_info->nr_pages, max_pfn);
	totalram_pages   = bs.current_pages;
#else 
	rc = HYPERVISOR_memory_op(XENMEM_get_pod_target, &pod_target);
	/*
	 * Xen prior to 3.4.0 masks the memory_op command to 4 bits, thus
	 * converting XENMEM_get_pod_target to XENMEM_decrease_reservation.
	 * Fortunately this results in a request with all input fields zero,
	 * but (due to the way bit 4 and upwards get interpreted) a starting
	 * extent of 1. When start_extent > nr_extents (>= in newer Xen), we
	 * simply get start_extent returned.
	 */
	totalram_bias = HYPERVISOR_memory_op(rc != -ENOSYS && rc != 1
		? XENMEM_maximum_reservation : XENMEM_current_reservation,
		&pod_target.domid);
	if ((long)totalram_bias != -ENOSYS) {
		BUG_ON(totalram_bias < totalram_pages);
		bs.current_pages = totalram_bias;
		totalram_bias -= totalram_pages;
	} else {
		totalram_bias = 0;
		bs.current_pages = totalram_pages;
	}
#endif
	bs.target_pages  = bs.current_pages;
	bs.balloon_low   = 0;
	bs.balloon_high  = 0;
	bs.driver_pages  = 0UL;
	init_timer(&balloon_timer);
	balloon_timer.data = 0;
	balloon_timer.function = balloon_alarm;

#ifdef CONFIG_PROC_FS
	if ((balloon_pde = create_xen_proc_entry("balloon", 0644)) == NULL) {
		WPRINTK("Unable to create /proc/xen/balloon.\n");
		return -1;
	}

	balloon_pde->read_proc  = balloon_read;
	balloon_pde->write_proc = balloon_write;
#endif
	balloon_sysfs_init();

#if defined(CONFIG_X86) && defined(CONFIG_XEN) 
	/* Initialise the balloon with excess memory space. */
	for (pfn = xen_start_info->nr_pages; pfn < max_pfn; pfn++) {
		page = pfn_to_page(pfn);
		if (!PageReserved(page)) {
			SetPageReserved(page);
			set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
			balloon_append(page, 0);
		}
	}
#endif

	target_watch.callback = watch_target;
	xenstore_notifier.notifier_call = balloon_init_watcher;

	register_xenstore_notifier(&xenstore_notifier);
    
	return 0;
}

subsys_initcall(balloon_init);

static void __exit balloon_exit(void)
{
	balloon_sysfs_exit();
	/* XXX - release balloon here */
}

module_exit(balloon_exit); 

void balloon_update_driver_allowance(long delta)
{
	unsigned long flags;

	balloon_lock(flags);
	bs.driver_pages += delta;
	balloon_unlock(flags);
}
EXPORT_SYMBOL_GPL(balloon_update_driver_allowance);

#if defined(CONFIG_XEN_BACKEND) || defined(CONFIG_XEN_BACKEND_MODULE)

#ifdef CONFIG_XEN
static int dealloc_pte_fn(
	pte_t *pte, struct page *pmd_page, unsigned long addr, void *data)
{
	unsigned long pfn, mfn = pte_mfn(*pte);
	int ret;
	struct xen_memory_reservation reservation = {
		.nr_extents   = 1,
		.extent_order = 0,
		.domid        = DOMID_SELF
	};
	set_xen_guest_handle(reservation.extent_start, &mfn);
	set_pte_at(&init_mm, addr, pte, __pte_ma(0));
	pfn = __pa(addr) >> PAGE_SHIFT;
	set_phys_to_machine(pfn, INVALID_P2M_ENTRY);
	SetPageReserved(pfn_to_page(pfn));
	ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation, &reservation);
	BUG_ON(ret != 1);
	return 0;
}
#endif

struct page **alloc_empty_pages_and_pagevec(int nr_pages)
{
	unsigned long flags;
	void *v;
	struct page *page, **pagevec;
	int i, ret;

	pagevec = kmalloc(sizeof(page) * nr_pages, GFP_KERNEL);
	if (pagevec == NULL)
		return NULL;

	for (i = 0; i < nr_pages; i++) {
		balloon_lock(flags);
		page = balloon_first_page();
		if (page && !PageHighMem(page)) {
			UNLIST_PAGE(page);
			bs.balloon_low--;
			balloon_unlock(flags);
			pagevec[i] = page;
			continue;
		}
		balloon_unlock(flags);

		page = pagevec[i] = alloc_page(GFP_KERNEL|__GFP_NOTRACK|__GFP_COLD);
		if (page == NULL)
			goto err;

		v = page_address(page);
		xen_scrub_pages(v, 1);

		balloon_lock(flags);

		if (xen_feature(XENFEAT_auto_translated_physmap)) {
			unsigned long gmfn = page_to_pfn(page);
			struct xen_memory_reservation reservation = {
				.nr_extents   = 1,
				.extent_order = 0,
				.domid        = DOMID_SELF
			};
			set_xen_guest_handle(reservation.extent_start, &gmfn);
			ret = HYPERVISOR_memory_op(XENMEM_decrease_reservation,
						   &reservation);
			if (ret == 1)
				ret = 0; /* success */
		} else {
#ifdef CONFIG_XEN
			ret = apply_to_page_range(&init_mm, (unsigned long)v,
						  PAGE_SIZE, dealloc_pte_fn,
						  NULL);
#else
			/* Cannot handle non-auto translate mode. */
			ret = 1;
#endif
		}

		if (ret != 0) {
			balloon_free_page(page);
			balloon_free_and_unlock(flags);
			goto err;
		}

		totalram_pages = --bs.current_pages - totalram_bias;
		if (PageHighMem(page))
			dec_totalhigh_pages();
		page_zone(page)->present_pages--;

		balloon_unlock(flags);
	}

 out:
	schedule_work(&balloon_worker);
#ifdef CONFIG_XEN
	flush_tlb_all();
#endif
	return pagevec;

 err:
	balloon_lock(flags);
	while (--i >= 0)
		balloon_append(pagevec[i], 0);
	balloon_unlock(flags);
	kfree(pagevec);
	pagevec = NULL;
	goto out;
}
EXPORT_SYMBOL_GPL(alloc_empty_pages_and_pagevec);

#endif /* CONFIG_XEN_BACKEND */

#ifdef CONFIG_XEN
static void _free_empty_pages(struct page **pagevec, int nr_pages,
			      bool account)
{
	unsigned long flags;
	int i;

	if (pagevec == NULL)
		return;
	balloon_lock(flags);
	for (i = 0; i < nr_pages; i++) {
		BUG_ON(page_count(pagevec[i]) != 1);
		balloon_append(pagevec[i], account);
	}
	if (account) {
		bs.current_pages -= nr_pages;
		totalram_pages = bs.current_pages - totalram_bias;
	}
	balloon_unlock(flags);

	schedule_work(&balloon_worker);
}

void free_empty_pages(struct page **pagevec, int nr_pages)
{
	_free_empty_pages(pagevec, nr_pages, true);
}
#endif

#if defined(CONFIG_XEN_BACKEND) || defined(CONFIG_XEN_BACKEND_MODULE)
void free_empty_pages_and_pagevec(struct page **pagevec, int nr_pages)
{
	if (pagevec) {
		_free_empty_pages(pagevec, nr_pages, false);
		kfree(pagevec);
	}
}
EXPORT_SYMBOL_GPL(free_empty_pages_and_pagevec);
#endif

void balloon_release_driver_page(struct page *page)
{
	unsigned long flags;

	balloon_lock(flags);
	balloon_append(page, 1);
	totalram_pages = --bs.current_pages - totalram_bias;
	bs.driver_pages--;
	balloon_unlock(flags);

	schedule_work(&balloon_worker);
}
EXPORT_SYMBOL_GPL(balloon_release_driver_page);

MODULE_LICENSE("Dual BSD/GPL");

/******************************************************************************
 * hypervisor.h
 * 
 * Linux-specific hypervisor handling.
 * 
 * Copyright (c) 2002-2004, K A Fraser
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

#ifndef __HYPERVISOR_H__
#define __HYPERVISOR_H__

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <xen/interface/xen.h>
#include <xen/interface/platform.h>
#include <xen/interface/event_channel.h>
#include <xen/interface/physdev.h>
#include <xen/interface/sched.h>
#include <xen/interface/nmi.h>
#include <asm/percpu.h>
#include <asm/ptrace.h>
#include <asm/page.h>

extern shared_info_t *HYPERVISOR_shared_info;

#define vcpu_info(cpu) (HYPERVISOR_shared_info->vcpu_info + (cpu))
#ifdef CONFIG_SMP
#define current_vcpu_info() vcpu_info(smp_processor_id())
#else
#define current_vcpu_info() vcpu_info(0)
#endif

#ifdef CONFIG_X86_32
extern unsigned long hypervisor_virt_start;
#endif

/* arch/xen/i386/kernel/setup.c */
extern start_info_t *xen_start_info;
#ifdef CONFIG_XEN_PRIVILEGED_GUEST
#define is_initial_xendomain() (xen_start_info->flags & SIF_INITDOMAIN)
#else
#define is_initial_xendomain() 0
#endif

#define init_hypervisor(c) ((void)((c)->x86_hyper_vendor = X86_HYPER_VENDOR_XEN))

/* arch/xen/kernel/evtchn.c */
/* Force a proper event-channel callback from Xen. */
void force_evtchn_callback(void);

/* arch/xen/kernel/process.c */
void xen_cpu_idle (void);

/* arch/xen/i386/kernel/hypervisor.c */
void do_hypervisor_callback(struct pt_regs *regs);

/* arch/xen/i386/mm/hypervisor.c */
/*
 * NB. ptr values should be PHYSICAL, not MACHINE. 'vals' should be already
 * be MACHINE addresses.
 */

void xen_pt_switch(pgd_t *);
void xen_new_user_pt(pgd_t *); /* x86_64 only */
void xen_load_gs(unsigned int selector); /* x86_64 only */
void xen_tlb_flush(void);
void xen_invlpg(unsigned long ptr);

void xen_l1_entry_update(pte_t *ptr, pte_t val);
void xen_l2_entry_update(pmd_t *ptr, pmd_t val);
void xen_l3_entry_update(pud_t *ptr, pud_t val); /* x86_64/PAE */
void xen_l4_entry_update(pgd_t *ptr, pgd_t val); /* x86_64 only */
void xen_pgd_pin(pgd_t *);
void xen_pgd_unpin(pgd_t *);

void xen_init_pgd_pin(void);

void xen_set_ldt(const void *ptr, unsigned int ents);

#ifdef CONFIG_SMP
#include <linux/cpumask.h>
void xen_tlb_flush_all(void);
void xen_invlpg_all(unsigned long ptr);
void xen_tlb_flush_mask(cpumask_t *mask);
void xen_invlpg_mask(cpumask_t *mask, unsigned long ptr);
#endif

/* Returns zero on success else negative errno. */
int xen_create_contiguous_region(
    unsigned long vstart, unsigned int order, unsigned int address_bits);
void xen_destroy_contiguous_region(
    unsigned long vstart, unsigned int order);

struct page;

int xen_limit_pages_to_max_mfn(
	struct page *pages, unsigned int order, unsigned int address_bits);

/* Turn jiffies into Xen system time. */
u64 jiffies_to_st(unsigned long jiffies);

#ifdef CONFIG_XEN_SCRUB_PAGES
void scrub_pages(void *, unsigned int);
void xen_scrub_pages(void *, unsigned int);
#else
#define scrub_pages(_p,_n) ((void)0)
#define xen_scrub_pages(_p,_n) ((void)0)
#endif

#ifdef CONFIG_XEN

DECLARE_PER_CPU(bool, xen_lazy_mmu);

int xen_multicall_flush(bool);

int __must_check xen_multi_update_va_mapping(unsigned long va, pte_t,
					     unsigned long flags);
int __must_check xen_multi_mmu_update(mmu_update_t *, unsigned int count,
				      unsigned int *success_count, domid_t);
int __must_check xen_multi_mmuext_op(struct mmuext_op *, unsigned int count,
				     unsigned int *success_count, domid_t);

#define __HAVE_ARCH_ENTER_LAZY_MMU_MODE
static inline void arch_enter_lazy_mmu_mode(void)
{
	__get_cpu_var(xen_lazy_mmu) = true;
}

static inline void arch_leave_lazy_mmu_mode(void)
{
	__get_cpu_var(xen_lazy_mmu) = false;
	xen_multicall_flush(false);
}

#if defined(CONFIG_X86_32)
#define arch_use_lazy_mmu_mode() unlikely(x86_read_percpu(xen_lazy_mmu))
#elif !defined(arch_use_lazy_mmu_mode)
#define arch_use_lazy_mmu_mode() unlikely(__get_cpu_var(xen_lazy_mmu))
#endif

struct gnttab_map_grant_ref;
bool gnttab_pre_map_adjust(unsigned int cmd, struct gnttab_map_grant_ref *,
			   unsigned int count);
#if CONFIG_XEN_COMPAT < 0x030400
int gnttab_post_map_adjust(const struct gnttab_map_grant_ref *, unsigned int);
#else
static inline int gnttab_post_map_adjust(const struct gnttab_map_grant_ref *m,
					 unsigned int count)
{
	BUG();
	return -ENOSYS;
}
#endif

#else /* CONFIG_XEN */

static inline void xen_multicall_flush(bool ignore) {}
#define arch_use_lazy_mmu_mode() false
#define xen_multi_update_va_mapping(...) ({ BUG(); -ENOSYS; })
#define xen_multi_mmu_update(...) ({ BUG(); -ENOSYS; })
#define xen_multi_mmuext_op(...) ({ BUG(); -ENOSYS; })

#define gnttab_pre_map_adjust(...) false
#define gnttab_post_map_adjust(...) ({ BUG(); -ENOSYS; })

#endif /* CONFIG_XEN */

#if defined(CONFIG_X86_64)
#define MULTI_UVMFLAGS_INDEX 2
#define MULTI_UVMDOMID_INDEX 3
#else
#define MULTI_UVMFLAGS_INDEX 3
#define MULTI_UVMDOMID_INDEX 4
#endif

#ifdef CONFIG_XEN
#define is_running_on_xen() 1
extern char hypercall_page[PAGE_SIZE];
#else
extern char *hypercall_stubs;
#define is_running_on_xen() (!!hypercall_stubs)
#endif

#include <xen/hypercall.h>

static inline int
HYPERVISOR_yield(
	void)
{
	int rc = HYPERVISOR_sched_op(SCHEDOP_yield, NULL);

#if CONFIG_XEN_COMPAT <= 0x030002
	if (rc == -ENOSYS)
		rc = HYPERVISOR_sched_op_compat(SCHEDOP_yield, 0);
#endif

	return rc;
}

static inline int
HYPERVISOR_block(
	void)
{
	int rc = HYPERVISOR_sched_op(SCHEDOP_block, NULL);

#if CONFIG_XEN_COMPAT <= 0x030002
	if (rc == -ENOSYS)
		rc = HYPERVISOR_sched_op_compat(SCHEDOP_block, 0);
#endif

	return rc;
}

static inline void __noreturn
HYPERVISOR_shutdown(
	unsigned int reason)
{
	struct sched_shutdown sched_shutdown = {
		.reason = reason
	};

	VOID(HYPERVISOR_sched_op(SCHEDOP_shutdown, &sched_shutdown));
#if CONFIG_XEN_COMPAT <= 0x030002
	VOID(HYPERVISOR_sched_op_compat(SCHEDOP_shutdown, reason));
#endif
	/* Don't recurse needlessly. */
	BUG_ON(reason != SHUTDOWN_crash);
	for(;;);
}

static inline int __must_check
HYPERVISOR_poll(
	evtchn_port_t *ports, unsigned int nr_ports, u64 timeout)
{
	int rc;
	struct sched_poll sched_poll = {
		.nr_ports = nr_ports,
		.timeout = jiffies_to_st(timeout)
	};
	set_xen_guest_handle(sched_poll.ports, ports);

	rc = HYPERVISOR_sched_op(SCHEDOP_poll, &sched_poll);
#if CONFIG_XEN_COMPAT <= 0x030002
	if (rc == -ENOSYS)
		rc = HYPERVISOR_sched_op_compat(SCHEDOP_yield, 0);
#endif

	return rc;
}

static inline int __must_check
HYPERVISOR_poll_no_timeout(
	evtchn_port_t *ports, unsigned int nr_ports)
{
	int rc;
	struct sched_poll sched_poll = {
		.nr_ports = nr_ports
	};
	set_xen_guest_handle(sched_poll.ports, ports);

	rc = HYPERVISOR_sched_op(SCHEDOP_poll, &sched_poll);
#if CONFIG_XEN_COMPAT <= 0x030002
	if (rc == -ENOSYS)
		rc = HYPERVISOR_sched_op_compat(SCHEDOP_yield, 0);
#endif

	return rc;
}

#ifdef CONFIG_XEN

static inline void
MULTI_update_va_mapping(
    multicall_entry_t *mcl, unsigned long va,
    pte_t new_val, unsigned long flags)
{
    mcl->op = __HYPERVISOR_update_va_mapping;
    mcl->args[0] = va;
#if defined(CONFIG_X86_64)
    mcl->args[1] = new_val.pte;
#elif defined(CONFIG_X86_PAE)
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = new_val.pte_high;
#else
    mcl->args[1] = new_val.pte_low;
    mcl->args[2] = 0;
#endif
    mcl->args[MULTI_UVMFLAGS_INDEX] = flags;
}

static inline void
MULTI_mmu_update(multicall_entry_t *mcl, mmu_update_t *req,
		 unsigned int count, unsigned int *success_count,
		 domid_t domid)
{
    mcl->op = __HYPERVISOR_mmu_update;
    mcl->args[0] = (unsigned long)req;
    mcl->args[1] = count;
    mcl->args[2] = (unsigned long)success_count;
    mcl->args[3] = domid;
}

static inline void
MULTI_memory_op(multicall_entry_t *mcl, unsigned int cmd, void *arg)
{
	mcl->op = __HYPERVISOR_memory_op;
	mcl->args[0] = cmd;
	mcl->args[1] = (unsigned long)arg;
}

static inline void
MULTI_grant_table_op(multicall_entry_t *mcl, unsigned int cmd,
		     void *uop, unsigned int count)
{
    mcl->op = __HYPERVISOR_grant_table_op;
    mcl->args[0] = cmd;
    mcl->args[1] = (unsigned long)uop;
    mcl->args[2] = count;
}

#else /* !defined(CONFIG_XEN) */

/* Multicalls not supported for HVM guests. */
static inline void MULTI_bug(multicall_entry_t *mcl, ...)
{
	BUG_ON(mcl);
}
#define MULTI_update_va_mapping(a,b,c,d) ((void)0)
#define MULTI_mmu_update(a,b,c,d,e) ((void)0)
#define MULTI_memory_op(a,b,c) ((void)0)
#define MULTI_grant_table_op(a,b,c,d) ((void)0)

#endif

#ifdef LINUX
/* drivers/staging/rt28?0/ use Windows-style types, including VOID */
#undef VOID
#endif

#endif /* __HYPERVISOR_H__ */

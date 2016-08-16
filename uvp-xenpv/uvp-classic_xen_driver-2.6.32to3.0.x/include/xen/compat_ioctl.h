/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Copyright IBM Corp. 2007
 *
 * Authors: Jimi Xenidis <jimix@watson.ibm.com>
 *          Hollis Blanchard <hollisb@us.ibm.com>
 */

#ifndef __LINUX_XEN_COMPAT_H__ 
#define __LINUX_XEN_COMPAT_H__ 

#include <linux/compat.h>

extern int privcmd_ioctl_32(int fd, unsigned int cmd, unsigned long arg);
struct privcmd_mmap_32 {
	int num;
	domid_t dom;
	compat_uptr_t entry;
};

struct privcmd_mmapbatch_32 {
	int num;     /* number of pages to populate */
	domid_t dom; /* target domain */
	__u64 addr;  /* virtual address */
	compat_uptr_t arr; /* array of mfns - top nibble set on err */
};
#define IOCTL_PRIVCMD_MMAP_32                   \
	_IOC(_IOC_NONE, 'P', 2, sizeof(struct privcmd_mmap_32))
#define IOCTL_PRIVCMD_MMAPBATCH_32                  \
	_IOC(_IOC_NONE, 'P', 3, sizeof(struct privcmd_mmapbatch_32))

#endif /* __LINUX_XEN_COMPAT_H__ */

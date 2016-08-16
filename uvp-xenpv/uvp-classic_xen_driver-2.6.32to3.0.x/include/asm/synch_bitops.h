#ifndef __XEN_SYNCH_BITOPS_H__
#define __XEN_SYNCH_BITOPS_H__

/*
 * Copyright 1992, Linus Torvalds.
 * Heavily modified to provide guaranteed strong synchronisation
 * when communicating with Xen or other guest OSes running on other CPUs.
 */

#ifdef HAVE_XEN_PLATFORM_COMPAT_H
#include <xen/platform-compat.h>
#endif

#define ADDR (*(volatile long *) addr)

static __inline__ void synch_set_bit(int nr, volatile void * addr)
{
    __asm__ __volatile__ ( 
        "lock btsl %1,%0"
        : "+m" (ADDR) : "Ir" (nr) : "memory" );
}

static __inline__ void synch_clear_bit(int nr, volatile void * addr)
{
    __asm__ __volatile__ (
        "lock btrl %1,%0"
        : "+m" (ADDR) : "Ir" (nr) : "memory" );
}

static __inline__ void synch_change_bit(int nr, volatile void * addr)
{
    __asm__ __volatile__ (
        "lock btcl %1,%0"
        : "+m" (ADDR) : "Ir" (nr) : "memory" );
}

static __inline__ int synch_test_and_set_bit(int nr, volatile void * addr)
{
    int oldbit;
    __asm__ __volatile__ (
        "lock btsl %2,%1\n\tsbbl %0,%0"
        : "=r" (oldbit), "+m" (ADDR) : "Ir" (nr) : "memory");
    return oldbit;
}

static __inline__ int synch_test_and_clear_bit(int nr, volatile void * addr)
{
    int oldbit;
    __asm__ __volatile__ (
        "lock btrl %2,%1\n\tsbbl %0,%0"
        : "=r" (oldbit), "+m" (ADDR) : "Ir" (nr) : "memory");
    return oldbit;
}

static __inline__ int synch_test_and_change_bit(int nr, volatile void * addr)
{
    int oldbit;

    __asm__ __volatile__ (
        "lock btcl %2,%1\n\tsbbl %0,%0"
        : "=r" (oldbit), "+m" (ADDR) : "Ir" (nr) : "memory");
    return oldbit;
}

struct __synch_xchg_dummy { unsigned long a[100]; };
#define __synch_xg(x) ((struct __synch_xchg_dummy *)(x))

#define synch_cmpxchg(ptr, old, new) \
((__typeof__(*(ptr)))__synch_cmpxchg((ptr),\
                                     (unsigned long)(old), \
                                     (unsigned long)(new), \
                                     sizeof(*(ptr))))

static inline unsigned long __synch_cmpxchg(volatile void *ptr,
					    unsigned long old,
					    unsigned long new, int size)
{
	unsigned long prev;
	switch (size) {
	case 1:
		__asm__ __volatile__("lock; cmpxchgb %b1,%2"
				     : "=a"(prev)
				     : "q"(new), "m"(*__synch_xg(ptr)),
				       "0"(old)
				     : "memory");
		return prev;
	case 2:
		__asm__ __volatile__("lock; cmpxchgw %w1,%2"
				     : "=a"(prev)
				     : "r"(new), "m"(*__synch_xg(ptr)),
				       "0"(old)
				     : "memory");
		return prev;
#ifdef CONFIG_X86_64
	case 4:
		__asm__ __volatile__("lock; cmpxchgl %k1,%2"
				     : "=a"(prev)
				     : "r"(new), "m"(*__synch_xg(ptr)),
				       "0"(old)
				     : "memory");
		return prev;
	case 8:
		__asm__ __volatile__("lock; cmpxchgq %1,%2"
				     : "=a"(prev)
				     : "r"(new), "m"(*__synch_xg(ptr)),
				       "0"(old)
				     : "memory");
		return prev;
#else
	case 4:
		__asm__ __volatile__("lock; cmpxchgl %1,%2"
				     : "=a"(prev)
				     : "r"(new), "m"(*__synch_xg(ptr)),
				       "0"(old)
				     : "memory");
		return prev;
#endif
	}
	return old;
}

#define synch_test_bit test_bit

#define synch_cmpxchg_subword synch_cmpxchg

#endif /* __XEN_SYNCH_BITOPS_H__ */

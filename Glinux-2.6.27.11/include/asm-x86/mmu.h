#ifndef _ASM_X86_MMU_H
#define _ASM_X86_MMU_H

#include <linux/spinlock.h>
#include <linux/mutex.h>

/*
 * The x86 doesn't have a mmu context, but
 * we put the segment information here.
 *
 * cpu_vm_mask is used to optimize ldt flushing.
 */
typedef struct {
	struct desc_struct *ldt;
#ifdef CONFIG_X86_64
	rwlock_t ldtlock;
#endif
	int size;
	struct mutex lock;
	unsigned long vdso;

#ifdef CONFIG_X86_32
#if defined(CONFIG_PAX_PAGEEXEC) || defined(CONFIG_PAX_SEGMEXEC)
	unsigned long user_cs_base;
	unsigned long user_cs_limit;

#if defined(CONFIG_PAX_PAGEEXEC) && defined(CONFIG_SMP)
	cpumask_t cpu_user_cs_mask;
#endif

#endif
#endif

} mm_context_t;

#ifdef CONFIG_SMP
void leave_mm(int cpu);
#else
static inline void leave_mm(int cpu)
{
}
#endif

#endif /* _ASM_X86_MMU_H */

/*
 *  Copyright (C) 1995  Linus Torvalds
 *  Copyright (C) 2001, 2002 Andi Kleen, SuSE Labs.
 *  Copyright (C) 2008-2009, Red Hat Inc., Ingo Molnar
 */
#include <linux/interrupt.h>
#include <linux/mmiotrace.h>
#include <linux/bootmem.h>
#include <linux/compiler.h>
#include <linux/highmem.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/vt_kern.h>
#include <linux/signal.h>
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/kdebug.h>
#include <linux/errno.h>
#include <linux/magic.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/mman.h>
#include <linux/tty.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/unistd.h>
#include <linux/compiler.h>

#include <asm-generic/sections.h>

#include <asm/tlbflush.h>
#include <asm/pgalloc.h>
#include <asm/segment.h>
#include <asm/system.h>
#include <asm/proto.h>
#include <asm/traps.h>
#include <asm/desc.h>
#include <asm/vsyscall.h>

/*
 * Page fault error code bits:
 *
 *   bit 0 ==	 0: no page found	1: protection fault
 *   bit 1 ==	 0: read access		1: write access
 *   bit 2 ==	 0: kernel-mode access	1: user-mode access
 *   bit 3 ==				1: use of reserved bit detected
 *   bit 4 ==				1: fault was an instruction fetch
 */
enum x86_pf_error_code {

	PF_PROT		=		1 << 0,
	PF_WRITE	=		1 << 1,
	PF_USER		=		1 << 2,
	PF_RSVD		=		1 << 3,
	PF_INSTR	=		1 << 4,
};

/*
 * Returns 0 if mmiotrace is disabled, or if the fault is not
 * handled by mmiotrace:
 */
static inline int kmmio_fault(struct pt_regs *regs, unsigned long addr)
{
	if (unlikely(is_kmmio_active()))
		if (kmmio_handler(regs, addr) == 1)
			return -1;
	return 0;
}

static inline int notify_page_fault(struct pt_regs *regs)
{
	int ret = 0;

	/* kprobe_running() needs smp_processor_id() */
	if (kprobes_built_in() && !user_mode(regs)) {
		preempt_disable();
		if (kprobe_running() && kprobe_fault_handler(regs, 14))
			ret = 1;
		preempt_enable();
	}

	return ret;
}

/*
 * Prefetch quirks:
 *
 * 32-bit mode:
 *
 *   Sometimes AMD Athlon/Opteron CPUs report invalid exceptions on prefetch.
 *   Check that here and ignore it.
 *
 * 64-bit mode:
 *
 *   Sometimes the CPU reports invalid exceptions on prefetch.
 *   Check that here and ignore it.
 *
 * Opcode checker based on code by Richard Brunner.
 */
static inline int
check_prefetch_opcode(struct pt_regs *regs, unsigned char *instr,
		      unsigned char opcode, int *prefetch)
{
	unsigned char instr_hi = opcode & 0xf0;
	unsigned char instr_lo = opcode & 0x0f;

	switch (instr_hi) {
	case 0x20:
	case 0x30:
		/*
		 * Values 0x26,0x2E,0x36,0x3E are valid x86 prefixes.
		 * In X86_64 long mode, the CPU will signal invalid
		 * opcode if some of these prefixes are present so
		 * X86_64 will never get here anyway
		 */
		return ((instr_lo & 7) == 0x6);
#ifdef CONFIG_X86_64
	case 0x40:
		/*
		 * In AMD64 long mode 0x40..0x4F are valid REX prefixes
		 * Need to figure out under what instruction mode the
		 * instruction was issued. Could check the LDT for lm,
		 * but for now it's good enough to assume that long
		 * mode only uses well known segments or kernel.
		 */
		return (!user_mode(regs)) || (regs->cs == __USER_CS);
#endif
	case 0x60:
		/* 0x64 thru 0x67 are valid prefixes in all modes. */
		return (instr_lo & 0xC) == 0x4;
	case 0xF0:
		/* 0xF0, 0xF2, 0xF3 are valid prefixes in all modes. */
		return !instr_lo || (instr_lo>>1) == 1;
	case 0x00:
		/* Prefetch instruction is 0x0F0D or 0x0F18 */
		if (probe_kernel_address(instr, opcode))
			return 0;

		*prefetch = (instr_lo == 0xF) &&
			(opcode == 0x0D || opcode == 0x18);
		return 0;
	default:
		return 0;
	}
}

static int
is_prefetch(struct pt_regs *regs, unsigned long error_code, unsigned long addr)
{
	unsigned char *max_instr;
	unsigned char *instr;
	int prefetch = 0;

	/*
	 * If it was a exec (instruction fetch) fault on NX page, then
	 * do not ignore the fault:
	 */
	if (error_code & PF_INSTR)
		return 0;

	instr = (void *)convert_ip_to_linear(current, regs);
	max_instr = instr + 15;

	if (user_mode(regs) && instr >= (unsigned char *)TASK_SIZE)
		return 0;

	while (instr < max_instr) {
		unsigned char opcode;

		if (probe_kernel_address(instr, opcode))
			break;

		instr++;

		if (!check_prefetch_opcode(regs, instr, opcode, &prefetch))
			break;
	}
	return prefetch;
}

static void
force_sig_info_fault(int si_signo, int si_code, unsigned long address,
		     struct task_struct *tsk)
{
	siginfo_t info;

	info.si_signo	= si_signo;
	info.si_errno	= 0;
	info.si_code	= si_code;
	info.si_addr	= (void __user *)address;

	force_sig_info(si_signo, &info, tsk);
}

#ifdef CONFIG_PAX_EMUTRAMP
static int pax_handle_fetch_fault(struct pt_regs *regs);
#endif

#ifdef CONFIG_PAX_PAGEEXEC
static inline pmd_t * pax_get_pmd(struct mm_struct *mm, unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, address);
	if (!pgd_present(*pgd))
		return NULL;
	pud = pud_offset(pgd, address);
	if (!pud_present(*pud))
		return NULL;
	pmd = pmd_offset(pud, address);
	if (!pmd_present(*pmd))
		return NULL;
	return pmd;
}
#endif

DEFINE_SPINLOCK(pgd_lock);
LIST_HEAD(pgd_list);

#ifdef CONFIG_X86_32
static inline pmd_t *vmalloc_sync_one(pgd_t *pgd, unsigned long address)
{
	unsigned index = pgd_index(address);
	pgd_t *pgd_k;
	pud_t *pud, *pud_k;
	pmd_t *pmd, *pmd_k;

	pgd += index;
	pgd_k = init_mm.pgd + index;

	if (!pgd_present(*pgd_k))
		return NULL;

	/*
	 * set_pgd(pgd, *pgd_k); here would be useless on PAE
	 * and redundant with the set_pmd() on non-PAE. As would
	 * set_pud.
	 */
	pud = pud_offset(pgd, address);
	pud_k = pud_offset(pgd_k, address);
	if (!pud_present(*pud_k))
		return NULL;

	pmd = pmd_offset(pud, address);
	pmd_k = pmd_offset(pud_k, address);
	if (!pmd_present(*pmd_k))
		return NULL;

	if (!pmd_present(*pmd)) {
		set_pmd(pmd, *pmd_k);
		arch_flush_lazy_mmu_mode();
	} else {
		BUG_ON(pmd_page(*pmd) != pmd_page(*pmd_k));
	}

	return pmd_k;
}

void vmalloc_sync_all(void)
{
	unsigned long address;

	if (SHARED_KERNEL_PMD)
		return;

	for (address = VMALLOC_START & PMD_MASK;
	     address >= TASK_SIZE && address < FIXADDR_TOP;
	     address += PMD_SIZE) {

		unsigned long flags;
		struct page *page;

		spin_lock_irqsave(&pgd_lock, flags);
		list_for_each_entry(page, &pgd_list, lru) {
			if (!vmalloc_sync_one(page_address(page), address))
				break;
		}
		spin_unlock_irqrestore(&pgd_lock, flags);
	}
}

/*
 * 32-bit:
 *
 *   Handle a fault on the vmalloc or module mapping area
 */
static noinline int vmalloc_fault(unsigned long address)
{
	unsigned long pgd_paddr;
	pmd_t *pmd_k;
	pte_t *pte_k;

	/* Make sure we are in vmalloc area: */
	if (!(address >= VMALLOC_START && address < VMALLOC_END))
		return -1;

	/*
	 * Synchronize this task's top level page-table
	 * with the 'reference' page table.
	 *
	 * Do _not_ use "current" here. We might be inside
	 * an interrupt in the middle of a task switch..
	 */
	pgd_paddr = read_cr3();
	pmd_k = vmalloc_sync_one(__va(pgd_paddr), address);
	if (!pmd_k)
		return -1;

	pte_k = pte_offset_kernel(pmd_k, address);
	if (!pte_present(*pte_k))
		return -1;

	return 0;
}

/*
 * Did it hit the DOS screen memory VA from vm86 mode?
 */
static inline void
check_v8086_mode(struct pt_regs *regs, unsigned long address,
		 struct task_struct *tsk)
{
	unsigned long bit;

	if (!v8086_mode(regs))
		return;

	bit = (address - 0xA0000) >> PAGE_SHIFT;
	if (bit < 32)
		tsk->thread.screen_bitmap |= 1 << bit;
}

static void dump_pagetable(unsigned long address)
{
	__typeof__(pte_val(__pte(0))) page;

	page = read_cr3();
	page = ((__typeof__(page) *) __va(page))[address >> PGDIR_SHIFT];

#ifdef CONFIG_X86_PAE
	printk("*pdpt = %016Lx ", page);
	if ((page >> PAGE_SHIFT) < max_low_pfn
	    && page & _PAGE_PRESENT) {
		page &= PAGE_MASK;
		page = ((__typeof__(page) *) __va(page))[(address >> PMD_SHIFT)
							& (PTRS_PER_PMD - 1)];
		printk(KERN_CONT "*pde = %016Lx ", page);
		page &= ~_PAGE_NX;
	}
#else
	printk("*pde = %08lx ", page);
#endif

	/*
	 * We must not directly access the pte in the highpte
	 * case if the page table is located in highmem.
	 * And let's rather not kmap-atomic the pte, just in case
	 * it's allocated already:
	 */
	if ((page >> PAGE_SHIFT) < max_low_pfn
	    && (page & _PAGE_PRESENT)
	    && !(page & _PAGE_PSE)) {

		page &= PAGE_MASK;
		page = ((__typeof__(page) *) __va(page))[(address >> PAGE_SHIFT)
							& (PTRS_PER_PTE - 1)];
		printk("*pte = %0*Lx ", sizeof(page)*2, (u64)page);
	}

	printk("\n");
}

#else /* CONFIG_X86_64: */

void vmalloc_sync_all(void)
{
	unsigned long address;

	for (address = VMALLOC_START & PGDIR_MASK; address <= VMALLOC_END;
	     address += PGDIR_SIZE) {

		const pgd_t *pgd_ref = pgd_offset_k(address);
		unsigned long flags;
		struct page *page;

		if (pgd_none(*pgd_ref))
			continue;

		spin_lock_irqsave(&pgd_lock, flags);
		list_for_each_entry(page, &pgd_list, lru) {
			pgd_t *pgd;
			pgd = (pgd_t *)page_address(page) + pgd_index(address);
			if (pgd_none(*pgd))
				set_pgd(pgd, *pgd_ref);
			else
				BUG_ON(pgd_page_vaddr(*pgd) != pgd_page_vaddr(*pgd_ref));
		}
		spin_unlock_irqrestore(&pgd_lock, flags);
	}
}

/*
 * 64-bit:
 *
 *   Handle a fault on the vmalloc area
 *
 * This assumes no large pages in there.
 */
static noinline int vmalloc_fault(unsigned long address)
{
	pgd_t *pgd, *pgd_ref;
	pud_t *pud, *pud_ref;
	pmd_t *pmd, *pmd_ref;
	pte_t *pte, *pte_ref;

	/* Make sure we are in vmalloc area: */
	if (!(address >= VMALLOC_START && address < VMALLOC_END))
		return -1;

	/*
	 * Copy kernel mappings over when needed. This can also
	 * happen within a race in page table update. In the later
	 * case just flush:
	 */
	pgd = pgd_offset(current->active_mm, address);
	pgd_ref = pgd_offset_k(address);
	if (pgd_none(*pgd_ref))
		return -1;

	if (pgd_none(*pgd))
		set_pgd(pgd, *pgd_ref);
	else
		BUG_ON(pgd_page_vaddr(*pgd) != pgd_page_vaddr(*pgd_ref));

	/*
	 * Below here mismatches are bugs because these lower tables
	 * are shared:
	 */

	pud = pud_offset(pgd, address);
	pud_ref = pud_offset(pgd_ref, address);
	if (pud_none(*pud_ref))
		return -1;

	if (pud_none(*pud) || pud_page_vaddr(*pud) != pud_page_vaddr(*pud_ref))
		BUG();

	pmd = pmd_offset(pud, address);
	pmd_ref = pmd_offset(pud_ref, address);
	if (pmd_none(*pmd_ref))
		return -1;

	if (pmd_none(*pmd) || pmd_page(*pmd) != pmd_page(*pmd_ref))
		BUG();

	pte_ref = pte_offset_kernel(pmd_ref, address);
	if (!pte_present(*pte_ref))
		return -1;

	pte = pte_offset_kernel(pmd, address);

	/*
	 * Don't use pte_page here, because the mappings can point
	 * outside mem_map, and the NUMA hash lookup cannot handle
	 * that:
	 */
	if (!pte_present(*pte) || pte_pfn(*pte) != pte_pfn(*pte_ref))
		BUG();

	return 0;
}

static const char errata93_warning[] =
KERN_ERR "******* Your BIOS seems to not contain a fix for K8 errata #93\n"
KERN_ERR "******* Working around it, but it may cause SEGVs or burn power.\n"
KERN_ERR "******* Please consider a BIOS update.\n"
KERN_ERR "******* Disabling USB legacy in the BIOS may also help.\n";

/*
 * No vm86 mode in 64-bit mode:
 */
static inline void
check_v8086_mode(struct pt_regs *regs, unsigned long address,
		 struct task_struct *tsk)
{
}

static int bad_address(void *p)
{
	unsigned long dummy;

	return probe_kernel_address((unsigned long *)p, dummy);
}

static void dump_pagetable(unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;

	pgd = (pgd_t *)read_cr3();

	pgd = __va((unsigned long)pgd & PHYSICAL_PAGE_MASK);

	pgd += pgd_index(address);
	if (bad_address(pgd))
		goto bad;

	printk("PGD %lx ", pgd_val(*pgd));

	if (!pgd_present(*pgd))
		goto out;

	pud = pud_offset(pgd, address);
	if (bad_address(pud))
		goto bad;

	printk("PUD %lx ", pud_val(*pud));
	if (!pud_present(*pud) || pud_large(*pud))
		goto out;

	pmd = pmd_offset(pud, address);
	if (bad_address(pmd))
		goto bad;

	printk("PMD %lx ", pmd_val(*pmd));
	if (!pmd_present(*pmd) || pmd_large(*pmd))
		goto out;

	pte = pte_offset_kernel(pmd, address);
	if (bad_address(pte))
		goto bad;

	printk("PTE %lx", pte_val(*pte));
out:
	printk("\n");
	return;
bad:
	printk("BAD\n");
}

#endif /* CONFIG_X86_64 */

/*
 * Workaround for K8 erratum #93 & buggy BIOS.
 *
 * BIOS SMM functions are required to use a specific workaround
 * to avoid corruption of the 64bit RIP register on C stepping K8.
 *
 * A lot of BIOS that didn't get tested properly miss this.
 *
 * The OS sees this as a page fault with the upper 32bits of RIP cleared.
 * Try to work around it here.
 *
 * Note we only handle faults in kernel here.
 * Does nothing on 32-bit.
 */
static int is_errata93(struct pt_regs *regs, unsigned long address)
{
#ifdef CONFIG_X86_64
	static int once;

	if (address != regs->ip)
		return 0;

	if ((address >> 32) != 0)
		return 0;

	address |= 0xffffffffUL << 32;
	if ((address >= (u64)_stext && address <= (u64)_etext) ||
	    (address >= MODULES_VADDR && address <= MODULES_END)) {
		if (!once) {
			printk(errata93_warning);
			once = 1;
		}
		regs->ip = address;
		return 1;
	}
#endif
	return 0;
}

/*
 * Work around K8 erratum #100 K8 in compat mode occasionally jumps
 * to illegal addresses >4GB.
 *
 * We catch this in the page fault handler because these addresses
 * are not reachable. Just detect this case and return.  Any code
 * segment in LDT is compatibility mode.
 */
static int is_errata100(struct pt_regs *regs, unsigned long address)
{
#ifdef CONFIG_X86_64
	if ((regs->cs == __USER32_CS || (regs->cs & SEGMENT_LDT)) && (address >> 32))
		return 1;
#endif
	return 0;
}

static int is_f00f_bug(struct pt_regs *regs, unsigned long address)
{
#ifdef CONFIG_X86_F00F_BUG
	unsigned long nr;

	/*
	 * Pentium F0 0F C7 C8 bug workaround:
	 */
	if (boot_cpu_data.f00f_bug) {
		nr = (address - idt_descr.address) >> 3;

		if (nr == 6) {
			do_invalid_op(regs, 0);
			return 1;
		}
	}
#endif
	return 0;
}

static const char nx_warning[] = KERN_CRIT
"kernel tried to execute NX-protected page - exploit attempt? (uid: %d, task: %s, pid: %d)\n";

static void
show_fault_oops(struct pt_regs *regs, unsigned long error_code,
		unsigned long address)
{
	if (!oops_may_print())
		return;

	if (nx_enabled && (error_code & PF_INSTR)) {
		unsigned int level;

		pte_t *pte = lookup_address(address, &level);

		if (pte && pte_present(*pte) && !pte_exec(*pte))
			printk(nx_warning, current_uid(), current->comm, task_pid_nr(current));
	}

#ifdef CONFIG_PAX_KERNEXEC
#ifdef CONFIG_MODULES
	if (init_mm.start_code <= address && address < (unsigned long)MODULES_END)
#else
	if (init_mm.start_code <= address && address < init_mm.end_code)
#endif
	{
		if (current->signal->curr_ip)
			printk(KERN_ERR "PAX: From %u.%u.%u.%u: %s:%d, uid/euid: %u/%u, attempted to modify kernel code\n",
					 NIPQUAD(current->signal->curr_ip), current->comm, task_pid_nr(current), current_uid(), current_euid());
		else
			printk(KERN_ERR "PAX: %s:%d, uid/euid: %u/%u, attempted to modify kernel code\n",
					 current->comm, task_pid_nr(current), current_uid(), current_euid());
	}
#endif

	printk(KERN_ALERT "BUG: unable to handle kernel ");
	if (address < PAGE_SIZE)
		printk(KERN_CONT "NULL pointer dereference");
	else
		printk(KERN_CONT "paging request");

	printk(KERN_CONT " at %p\n", (void *) address);
	printk(KERN_ALERT "IP:");
	printk_address(regs->ip, 1);

	dump_pagetable(address);
}

static noinline void
pgtable_bad(struct pt_regs *regs, unsigned long error_code,
	    unsigned long address)
{
	struct task_struct *tsk;
	unsigned long flags;
	int sig;

	flags = oops_begin();
	tsk = current;
	sig = SIGKILL;

	printk(KERN_ALERT "%s: Corrupted page table at address %lx\n",
	       tsk->comm, address);
	dump_pagetable(address);

	tsk->thread.cr2		= address;
	tsk->thread.trap_no	= 14;
	tsk->thread.error_code	= error_code;

	if (__die("Bad pagetable", regs, error_code))
		sig = 0;

	oops_end(flags, regs, sig);
}

static noinline void
no_context(struct pt_regs *regs, unsigned long error_code,
	   unsigned long address)
{
	struct task_struct *tsk = current;
	unsigned long *stackend;
	unsigned long flags;
	int sig;

	/* Are we prepared to handle this kernel fault? */
	if (fixup_exception(regs))
		return;

	/*
	 * 32-bit:
	 *
	 *   Valid to do another page fault here, because if this fault
	 *   had been triggered by is_prefetch fixup_exception would have
	 *   handled it.
	 *
	 * 64-bit:
	 *
	 *   Hall of shame of CPU/BIOS bugs.
	 */
	if (is_prefetch(regs, error_code, address))
		return;

	if (is_errata93(regs, address))
		return;

	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice:
	 */
	flags = oops_begin();

	show_fault_oops(regs, error_code, address);

	stackend = end_of_stack(tsk);
	if (*stackend != STACK_END_MAGIC)
		printk(KERN_ALERT "Thread overran stack, or stack corrupted\n");

	tsk->thread.cr2		= address;
	tsk->thread.trap_no	= 14;
	tsk->thread.error_code	= error_code;

	sig = SIGKILL;
	if (__die("Oops", regs, error_code))
		sig = 0;

	/* Executive summary in case the body of the oops scrolled away */
	printk(KERN_EMERG "CR2: %016lx\n", address);

	oops_end(flags, regs, sig);
}

/*
 * Print out info about fatal segfaults, if the show_unhandled_signals
 * sysctl is set:
 */
static inline void
show_signal_msg(struct pt_regs *regs, unsigned long error_code,
		unsigned long address, struct task_struct *tsk)
{
	if (!unhandled_signal(tsk, SIGSEGV))
		return;

	if (!printk_ratelimit())
		return;

	printk(KERN_CONT "%s%s[%d]: segfault at %lx ip %p sp %p error %lx",
		task_pid_nr(tsk) > 1 ? KERN_INFO : KERN_EMERG,
		tsk->comm, task_pid_nr(tsk), address,
		(void *)regs->ip, (void *)regs->sp, error_code);

	print_vma_addr(KERN_CONT " in ", regs->ip);

	printk(KERN_CONT "\n");
}

static void
__bad_area_nosemaphore(struct pt_regs *regs, unsigned long error_code,
		       unsigned long address, int si_code)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm = tsk->mm;

#ifdef CONFIG_X86_64
	if (mm && (error_code & PF_INSTR)) {
		if (regs->ip == (unsigned long)vgettimeofday) {
			regs->ip = (unsigned long)VDSO64_SYMBOL(mm->context.vdso, fallback_gettimeofday);
			return;
		} else if (regs->ip == (unsigned long)vtime) {
			regs->ip = (unsigned long)VDSO64_SYMBOL(mm->context.vdso, fallback_time);
			return;
		} else if (regs->ip == (unsigned long)vgetcpu) {
			regs->ip = (unsigned long)VDSO64_SYMBOL(mm->context.vdso, getcpu);
			return;
		}
	}
#endif

#if defined(CONFIG_PAX_PAGEEXEC) || defined(CONFIG_PAX_SEGMEXEC)
	if (mm && (error_code & PF_USER)) {
		unsigned long ip = regs->ip;

		if (v8086_mode(regs))
			ip = ((regs->cs & 0xffff) << 4) + (regs->ip & 0xffff);

		/*
		 * It's possible to have interrupts off here:
		 */
		local_irq_enable();

#ifdef CONFIG_PAX_PAGEEXEC
		if ((mm->pax_flags & MF_PAX_PAGEEXEC) &&
		    ((nx_enabled && (error_code & PF_INSTR)) || (!(error_code & (PF_PROT | PF_WRITE)) && regs->ip == address))) {

#ifdef CONFIG_PAX_EMUTRAMP
			switch (pax_handle_fetch_fault(regs)) {
			case 2:
				return;
			}
#endif

			pax_report_fault(regs, (void *)regs->ip, (void *)regs->sp);
			do_group_exit(SIGKILL);
		}
#endif

#ifdef CONFIG_PAX_SEGMEXEC
		if ((mm->pax_flags & MF_PAX_SEGMEXEC) && !(error_code & (PF_PROT | PF_WRITE)) && (regs->ip + SEGMEXEC_TASK_SIZE == address)) {

#ifdef CONFIG_PAX_EMUTRAMP
			switch (pax_handle_fetch_fault(regs)) {
			case 2:
				return;
			}
#endif

			pax_report_fault(regs, (void *)regs->ip, (void *)regs->sp);
			do_group_exit(SIGKILL);
		}
#endif

	}
#endif

	/* User mode accesses just cause a SIGSEGV */
	if (error_code & PF_USER) {
		/*
		 * It's possible to have interrupts off here:
		 */
		local_irq_enable();

		/*
		 * Valid to do another page fault here because this one came
		 * from user space:
		 */
		if (is_prefetch(regs, error_code, address))
			return;

		if (is_errata100(regs, address))
			return;

		if (unlikely(show_unhandled_signals))
			show_signal_msg(regs, error_code, address, tsk);

		/* Kernel addresses are always protection faults: */
		tsk->thread.cr2		= address;
		tsk->thread.error_code	= error_code | (address >= TASK_SIZE);
		tsk->thread.trap_no	= 14;

		force_sig_info_fault(SIGSEGV, si_code, address, tsk);

		return;
	}

	if (is_f00f_bug(regs, address))
		return;

	no_context(regs, error_code, address);
}

static noinline void
bad_area_nosemaphore(struct pt_regs *regs, unsigned long error_code,
		     unsigned long address)
{
	__bad_area_nosemaphore(regs, error_code, address, SEGV_MAPERR);
}

static void
__bad_area(struct pt_regs *regs, unsigned long error_code,
	   unsigned long address, int si_code)
{
	struct mm_struct *mm = current->mm;

	/*
	 * Something tried to access memory that isn't in our memory map..
	 * Fix it, but check if it's kernel or user first..
	 */
	up_read(&mm->mmap_sem);

	__bad_area_nosemaphore(regs, error_code, address, si_code);
}

static noinline void
bad_area(struct pt_regs *regs, unsigned long error_code, unsigned long address)
{
	__bad_area(regs, error_code, address, SEGV_MAPERR);
}

static noinline void
bad_area_access_error(struct pt_regs *regs, unsigned long error_code,
		      unsigned long address)
{
	__bad_area(regs, error_code, address, SEGV_ACCERR);
}

/* TODO: fixup for "mm-invoke-oom-killer-from-page-fault.patch" */
static void
out_of_memory(struct pt_regs *regs, unsigned long error_code,
	      unsigned long address)
{
	/*
	 * We ran out of memory, call the OOM killer, and return the userspace
	 * (which will retry the fault, or kill us if we got oom-killed):
	 */
	up_read(&current->mm->mmap_sem);

	pagefault_out_of_memory();
}

static void
do_sigbus(struct pt_regs *regs, unsigned long error_code, unsigned long address)
{
	struct task_struct *tsk = current;
	struct mm_struct *mm = tsk->mm;

	up_read(&mm->mmap_sem);

	/* Kernel mode? Handle exceptions or die: */
	if (!(error_code & PF_USER))
		no_context(regs, error_code, address);

	/* User-space => ok to do another page fault: */
	if (is_prefetch(regs, error_code, address))
		return;

	tsk->thread.cr2		= address;
	tsk->thread.error_code	= error_code;
	tsk->thread.trap_no	= 14;

	force_sig_info_fault(SIGBUS, BUS_ADRERR, address, tsk);
}

static noinline void
mm_fault_error(struct pt_regs *regs, unsigned long error_code,
	       unsigned long address, unsigned int fault)
{
	if (fault & VM_FAULT_OOM) {
		out_of_memory(regs, error_code, address);
	} else {
		if (fault & VM_FAULT_SIGBUS)
			do_sigbus(regs, error_code, address);
		else
			BUG();
	}
}

static int spurious_fault_check(unsigned long error_code, pte_t *pte)
{
	if ((error_code & PF_WRITE) && !pte_write(*pte))
		return 0;

	if ((error_code & PF_INSTR) && !pte_exec(*pte))
		return 0;

	return 1;
}

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_PAGEEXEC)
static int pax_handle_pageexec_fault(struct pt_regs *regs, struct mm_struct *mm, unsigned long address, unsigned long error_code)
{
	pte_t *pte;
	pmd_t *pmd;
	spinlock_t *ptl;
	unsigned char pte_mask;

	if (nx_enabled || (error_code & (PF_PROT|PF_USER)) != (PF_PROT|PF_USER) || v8086_mode(regs) ||
	    !(mm->pax_flags & MF_PAX_PAGEEXEC))
		return 0;

	/* PaX: it's our fault, let's handle it if we can */

	/* PaX: take a look at read faults before acquiring any locks */
	if (unlikely(!(error_code & PF_WRITE) && (regs->ip == address))) {
		/* instruction fetch attempt from a protected page in user mode */
		up_read(&mm->mmap_sem);

#ifdef CONFIG_PAX_EMUTRAMP
		switch (pax_handle_fetch_fault(regs)) {
		case 2:
			return 1;
		}
#endif

		pax_report_fault(regs, (void *)regs->ip, (void *)regs->sp);
		do_group_exit(SIGKILL);
	}

	pmd = pax_get_pmd(mm, address);
	if (unlikely(!pmd))
		return 0;

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	if (unlikely(!(pte_val(*pte) & _PAGE_PRESENT) || pte_user(*pte))) {
		pte_unmap_unlock(pte, ptl);
		return 0;
	}

	if (unlikely((error_code & PF_WRITE) && !pte_write(*pte))) {
		/* write attempt to a protected page in user mode */
		pte_unmap_unlock(pte, ptl);
		return 0;
	}

#ifdef CONFIG_SMP
	if (likely(address > get_limit(regs->cs) && cpu_isset(smp_processor_id(), mm->context.cpu_user_cs_mask)))
#else
	if (likely(address > get_limit(regs->cs)))
#endif
	{
		set_pte(pte, pte_mkread(*pte));
		__flush_tlb_one(address);
		pte_unmap_unlock(pte, ptl);
		up_read(&mm->mmap_sem);
		return 1;
	}

	pte_mask = _PAGE_ACCESSED | _PAGE_USER | ((error_code & PF_WRITE) << (_PAGE_BIT_DIRTY-1));

	/*
	 * PaX: fill DTLB with user rights and retry
	 */
	__asm__ __volatile__ (
#ifdef CONFIG_PAX_MEMORY_UDEREF
		"movw %w4,%%es\n"
#endif
		"orb %2,(%1)\n"
#if defined(CONFIG_M586) || defined(CONFIG_M586TSC)
/*
 * PaX: let this uncommented 'invlpg' remind us on the behaviour of Intel's
 * (and AMD's) TLBs. namely, they do not cache PTEs that would raise *any*
 * page fault when examined during a TLB load attempt. this is true not only
 * for PTEs holding a non-present entry but also present entries that will
 * raise a page fault (such as those set up by PaX, or the copy-on-write
 * mechanism). in effect it means that we do *not* need to flush the TLBs
 * for our target pages since their PTEs are simply not in the TLBs at all.

 * the best thing in omitting it is that we gain around 15-20% speed in the
 * fast path of the page fault handler and can get rid of tracing since we
 * can no longer flush unintended entries.
 */
		"invlpg (%0)\n"
#endif
		"testb $0,%%es:(%0)\n"
		"xorb %3,(%1)\n"
#ifdef CONFIG_PAX_MEMORY_UDEREF
		"pushl %%ss\n"
		"popl %%es\n"
#endif
		:
		: "r" (address), "r" (pte), "q" (pte_mask), "i" (_PAGE_USER), "r" (__USER_DS)
		: "memory", "cc");
	pte_unmap_unlock(pte, ptl);
	up_read(&mm->mmap_sem);
	return 1;
}
#endif

/*
 * Handle a spurious fault caused by a stale TLB entry.
 *
 * This allows us to lazily refresh the TLB when increasing the
 * permissions of a kernel page (RO -> RW or NX -> X).  Doing it
 * eagerly is very expensive since that implies doing a full
 * cross-processor TLB flush, even if no stale TLB entries exist
 * on other processors.
 *
 * There are no security implications to leaving a stale TLB when
 * increasing the permissions on a page.
 */
static noinline int
spurious_fault(unsigned long error_code, unsigned long address)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int ret;

	/* Reserved-bit violation or user access to kernel space? */
	if (error_code & (PF_USER | PF_RSVD))
		return 0;

	pgd = init_mm.pgd + pgd_index(address);
	if (!pgd_present(*pgd))
		return 0;

	pud = pud_offset(pgd, address);
	if (!pud_present(*pud))
		return 0;

	if (pud_large(*pud))
		return spurious_fault_check(error_code, (pte_t *) pud);

	pmd = pmd_offset(pud, address);
	if (!pmd_present(*pmd))
		return 0;

	if (pmd_large(*pmd))
		return spurious_fault_check(error_code, (pte_t *) pmd);

	pte = pte_offset_kernel(pmd, address);
	if (!pte_present(*pte))
		return 0;

	ret = spurious_fault_check(error_code, pte);
	if (!ret)
		return 0;

	/*
	 * Make sure we have permissions in PMD.
	 * If not, then there's a bug in the page tables:
	 */
	ret = spurious_fault_check(error_code, (pte_t *) pmd);
	WARN_ONCE(!ret, "PMD has incorrect permission bits\n");

	return ret;
}

int show_unhandled_signals = 1;

static inline int
access_error(unsigned long error_code, int write, struct vm_area_struct *vma)
{
	if (nx_enabled && (error_code & PF_INSTR) && !(vma->vm_flags & VM_EXEC))
		return 1;

	if (write) {
		/* write, present and write, not present: */
		if (unlikely(!(vma->vm_flags & VM_WRITE)))
			return 1;
		return 0;
	}

	/* read, present: */
	if (unlikely(error_code & PF_PROT))
		return 1;

	/* read, not present: */
	if (unlikely(!(vma->vm_flags & (VM_READ | VM_EXEC | VM_WRITE))))
		return 1;

	return 0;
}

static int fault_in_kernel_space(unsigned long address)
{
	return address >= TASK_SIZE_MAX;
}

/*
 * This routine handles page faults.  It determines the address,
 * and the problem, and then passes it off to one of the appropriate
 * routines.
 */
dotraplinkage void __kprobes
do_page_fault(struct pt_regs *regs, unsigned long error_code)
{
	struct vm_area_struct *vma;
	struct task_struct *tsk;
	struct mm_struct *mm;
	int write;
	int fault;

	/* Get the faulting address: */
	const unsigned long address = read_cr2();

	tsk = current;
	mm = tsk->mm;

	prefetchw(&mm->mmap_sem);

	if (unlikely(kmmio_fault(regs, address)))
		return;

	/*
	 * We fault-in kernel-space virtual memory on-demand. The
	 * 'reference' page table is init_mm.pgd.
	 *
	 * NOTE! We MUST NOT take any locks for this case. We may
	 * be in an interrupt or a critical region, and should
	 * only copy the information from the master page table,
	 * nothing more.
	 *
	 * This verifies that the fault happens in kernel space
	 * (error_code & 4) == 0, and that the fault was not a
	 * protection error (error_code & 9) == 0.
	 */
	if (unlikely(fault_in_kernel_space(address))) {
		if (!(error_code & (PF_RSVD|PF_USER|PF_PROT)) &&
		    vmalloc_fault(address) >= 0)
			return;

		/* Can handle a stale RO->RW TLB: */
		if (spurious_fault(error_code, address))
			return;

		/* kprobes don't want to hook the spurious faults: */
		if (notify_page_fault(regs))
			return;
		/*
		 * Don't take the mm semaphore here. If we fixup a prefetch
		 * fault we could otherwise deadlock:
		 */
		bad_area_nosemaphore(regs, error_code, address);

		return;
	}

	/* kprobes don't want to hook the spurious faults: */
	if (unlikely(notify_page_fault(regs)))
		return;
	/*
	 * It's safe to allow irq's after cr2 has been saved and the
	 * vmalloc fault has been handled.
	 *
	 * User-mode registers count as a user access even for any
	 * potential system fault or CPU buglet:
	 */
	if (user_mode(regs)) {
		local_irq_enable();
		error_code |= PF_USER;
	} else {
		if (regs->flags & X86_EFLAGS_IF)
			local_irq_enable();
	}

	if (unlikely(error_code & PF_RSVD))
		pgtable_bad(regs, error_code, address);

	/*
	 * If we're in an interrupt, have no user context or are running
	 * in an atomic region then we must not take the fault:
	 */
	if (unlikely(in_atomic() || !mm)) {
		bad_area_nosemaphore(regs, error_code, address);
		return;
	}

	/*
	 * When running in the kernel we expect faults to occur only to
	 * addresses in user space.  All other faults represent errors in
	 * the kernel and should generate an OOPS.  Unfortunately, in the
	 * case of an erroneous fault occurring in a code path which already
	 * holds mmap_sem we will deadlock attempting to validate the fault
	 * against the address space.  Luckily the kernel only validly
	 * references user space from well defined areas of code, which are
	 * listed in the exceptions table.
	 *
	 * As the vast majority of faults will be valid we will only perform
	 * the source reference check when there is a possibility of a
	 * deadlock. Attempt to lock the address space, if we cannot we then
	 * validate the source. If this is invalid we can skip the address
	 * space check, thus avoiding the deadlock:
	 */
	if (unlikely(!down_read_trylock(&mm->mmap_sem))) {
		if ((error_code & PF_USER) == 0 &&
		    !search_exception_tables(regs->ip)) {
			bad_area_nosemaphore(regs, error_code, address);
			return;
		}
		down_read(&mm->mmap_sem);
	} else {
		/*
		 * The above down_read_trylock() might have succeeded in
		 * which case we'll have missed the might_sleep() from
		 * down_read():
		 */
		might_sleep();
	}

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_PAGEEXEC)
	if (pax_handle_pageexec_fault(regs, mm, address, error_code))
		return;
#endif

	vma = find_vma(mm, address);
	if (unlikely(!vma)) {
		bad_area(regs, error_code, address);
		return;
	}
	if (likely(vma->vm_start <= address))
		goto good_area;
	if (unlikely(!(vma->vm_flags & VM_GROWSDOWN))) {
		bad_area(regs, error_code, address);
		return;
	}
	/*
	 * Accessing the stack below %sp is always a bug.
	 * The large cushion allows instructions like enter
	 * and pusha to work. ("enter $65535, $31" pushes
	 * 32 pointers and then decrements %sp by 65535.)
	 */
	if (unlikely(address + 65536 + 32 * sizeof(unsigned long) < task_pt_regs(tsk)->sp)) {
		bad_area(regs, error_code, address);
		return;
	}

#ifdef CONFIG_PAX_SEGMEXEC
	if (unlikely((mm->pax_flags & MF_PAX_SEGMEXEC) && vma->vm_end - SEGMEXEC_TASK_SIZE - 1 < address - SEGMEXEC_TASK_SIZE - 1)) {
		bad_area(regs, error_code, address);
		return;
	}
#endif

	if (unlikely(expand_stack(vma, address))) {
		bad_area(regs, error_code, address);
		return;
	}

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
good_area:
	write = error_code & PF_WRITE;

	if (unlikely(access_error(error_code, write, vma))) {
		bad_area_access_error(regs, error_code, address);
		return;
	}

	/*
	 * If for any reason at all we couldn't handle the fault,
	 * make sure we exit gracefully rather than endlessly redo
	 * the fault:
	 */
	fault = handle_mm_fault(mm, vma, address, write);

	if (unlikely(fault & VM_FAULT_ERROR)) {
		mm_fault_error(regs, error_code, address, fault);
		return;
	}

	if (fault & VM_FAULT_MAJOR)
		tsk->maj_flt++;
	else
		tsk->min_flt++;

	check_v8086_mode(regs, address, tsk);

	up_read(&mm->mmap_sem);
}

#ifdef CONFIG_PAX_EMUTRAMP
static int pax_handle_fetch_fault_32(struct pt_regs *regs)
{
	int err;

	do { /* PaX: gcc trampoline emulation #1 */
		unsigned char mov1, mov2;
		unsigned short jmp;
		unsigned int addr1, addr2;

#ifdef CONFIG_X86_64
		if ((regs->ip + 11) >> 32)
			break;
#endif

		err = get_user(mov1, (unsigned char __user *)regs->ip);
		err |= get_user(addr1, (unsigned int __user *)(regs->ip + 1));
		err |= get_user(mov2, (unsigned char __user *)(regs->ip + 5));
		err |= get_user(addr2, (unsigned int __user *)(regs->ip + 6));
		err |= get_user(jmp, (unsigned short __user *)(regs->ip + 10));

		if (err)
			break;

		if (mov1 == 0xB9 && mov2 == 0xB8 && jmp == 0xE0FF) {
			regs->cx = addr1;
			regs->ax = addr2;
			regs->ip = addr2;
			return 2;
		}
	} while (0);

	do { /* PaX: gcc trampoline emulation #2 */
		unsigned char mov, jmp;
		unsigned int addr1, addr2;

#ifdef CONFIG_X86_64
		if ((regs->ip + 9) >> 32)
			break;
#endif

		err = get_user(mov, (unsigned char __user *)regs->ip);
		err |= get_user(addr1, (unsigned int __user *)(regs->ip + 1));
		err |= get_user(jmp, (unsigned char __user *)(regs->ip + 5));
		err |= get_user(addr2, (unsigned int __user *)(regs->ip + 6));

		if (err)
			break;

		if (mov == 0xB9 && jmp == 0xE9) {
			regs->cx = addr1;
			regs->ip = (unsigned int)(regs->ip + addr2 + 10);
			return 2;
		}
	} while (0);

	return 1; /* PaX in action */
}

#ifdef CONFIG_X86_64
static int pax_handle_fetch_fault_64(struct pt_regs *regs)
{
	int err;

	do { /* PaX: gcc trampoline emulation #1 */
		unsigned short mov1, mov2, jmp1;
		unsigned char jmp2;
		unsigned int addr1;
		unsigned long addr2;

		err = get_user(mov1, (unsigned short __user *)regs->ip);
		err |= get_user(addr1, (unsigned int __user *)(regs->ip + 2));
		err |= get_user(mov2, (unsigned short __user *)(regs->ip + 6));
		err |= get_user(addr2, (unsigned long __user *)(regs->ip + 8));
		err |= get_user(jmp1, (unsigned short __user *)(regs->ip + 16));
		err |= get_user(jmp2, (unsigned char __user *)(regs->ip + 18));

		if (err)
			break;

		if (mov1 == 0xBB41 && mov2 == 0xBA49 && jmp1 == 0xFF49 && jmp2 == 0xE3) {
			regs->r11 = addr1;
			regs->r10 = addr2;
			regs->ip = addr1;
			return 2;
		}
	} while (0);

	do { /* PaX: gcc trampoline emulation #2 */
		unsigned short mov1, mov2, jmp1;
		unsigned char jmp2;
		unsigned long addr1, addr2;

		err = get_user(mov1, (unsigned short __user *)regs->ip);
		err |= get_user(addr1, (unsigned long __user *)(regs->ip + 2));
		err |= get_user(mov2, (unsigned short __user *)(regs->ip + 10));
		err |= get_user(addr2, (unsigned long __user *)(regs->ip + 12));
		err |= get_user(jmp1, (unsigned short __user *)(regs->ip + 20));
		err |= get_user(jmp2, (unsigned char __user *)(regs->ip + 22));

		if (err)
			break;

		if (mov1 == 0xBB49 && mov2 == 0xBA49 && jmp1 == 0xFF49 && jmp2 == 0xE3) {
			regs->r11 = addr1;
			regs->r10 = addr2;
			regs->ip = addr1;
			return 2;
		}
	} while (0);

	return 1; /* PaX in action */
}
#endif

/*
 * PaX: decide what to do with offenders (regs->ip = fault address)
 *
 * returns 1 when task should be killed
 *         2 when gcc trampoline was detected
 */
static int pax_handle_fetch_fault(struct pt_regs *regs)
{
	if (v8086_mode(regs))
		return 1;

	if (!(current->mm->pax_flags & MF_PAX_EMUTRAMP))
		return 1;

#ifdef CONFIG_X86_32
	return pax_handle_fetch_fault_32(regs);
#else
	if (regs->cs == __USER32_CS || (regs->cs & SEGMENT_LDT))
		return pax_handle_fetch_fault_32(regs);
	else
		return pax_handle_fetch_fault_64(regs);
#endif
}
#endif

#if defined(CONFIG_PAX_PAGEEXEC) || defined(CONFIG_PAX_SEGMEXEC)
void pax_report_insns(void *pc, void *sp)
{
	long i;

	printk(KERN_ERR "PAX: bytes at PC: ");
	for (i = 0; i < 20; i++) {
		unsigned char c;
		if (get_user(c, (unsigned char __user *)pc+i))
			printk(KERN_CONT "?? ");
		else
			printk(KERN_CONT "%02x ", c);
	}
	printk("\n");

	printk(KERN_ERR "PAX: bytes at SP-%lu: ", (unsigned long)sizeof(long));
	for (i = -1; i < 80 / sizeof(long); i++) {
		unsigned long c;
		if (get_user(c, (unsigned long __user *)sp+i))
#ifdef CONFIG_X86_32
			printk(KERN_CONT "???????? ");
#else
			printk(KERN_CONT "???????????????? ");
#endif
		else
			printk(KERN_CONT "%0*lx ", 2 * (int)sizeof(long), c);
	}
	printk("\n");
}
#endif

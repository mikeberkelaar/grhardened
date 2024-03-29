/*
 * Suspend support specific for i386.
 *
 * Distribute under GPLv2
 *
 * Copyright (c) 2002 Pavel Machek <pavel@suse.cz>
 * Copyright (c) 2001 Patrick Mochel <mochel@osdl.org>
 */

#include <linux/module.h>
#include <linux/suspend.h>
#include <asm/mtrr.h>
#include <asm/mce.h>
#include <asm/xcr.h>
#include <asm/suspend.h>

static struct saved_context saved_context;

unsigned long saved_context_ebx;
unsigned long saved_context_esp, saved_context_ebp;
unsigned long saved_context_esi, saved_context_edi;
unsigned long saved_context_eflags;

static void __save_processor_state(struct saved_context *ctxt)
{
	mtrr_save_fixed_ranges(NULL);
	kernel_fpu_begin();

	/*
	 * descriptor tables
	 */
	store_gdt(&ctxt->gdt);
	store_idt(&ctxt->idt);
	store_tr(ctxt->tr);

	/*
	 * segment registers
	 */
	savesegment(es, ctxt->es);
	savesegment(fs, ctxt->fs);
	savesegment(gs, ctxt->gs);
	savesegment(ss, ctxt->ss);

	/*
	 * control registers
	 */
	ctxt->cr0 = read_cr0();
	ctxt->cr2 = read_cr2();
	ctxt->cr3 = read_cr3();
	ctxt->cr4 = read_cr4_safe();
}

/* Needed by apm.c */
void save_processor_state(void)
{
	__save_processor_state(&saved_context);
}
EXPORT_SYMBOL(save_processor_state);

static void do_fpu_end(void)
{
	/*
	 * Restore FPU regs if necessary.
	 */
	kernel_fpu_end();
}

static void fix_processor_context(void)
{
	int cpu = smp_processor_id();
	struct tss_struct *t = init_tss + cpu;

	set_tss_desc(cpu, t);	/*
				 * This just modifies memory; should not be
				 * necessary. But... This is necessary, because
				 * 386 hardware has concept of busy TSS or some
				 * similar stupidity.
				 */

	load_TR_desc();				/* This does ltr */
	load_LDT(&current->active_mm->context);	/* This does lldt */

	/*
	 * Now maybe reload the debug registers
	 */
	if (current->thread.debugreg7) {
		set_debugreg(current->thread.debugreg0, 0);
		set_debugreg(current->thread.debugreg1, 1);
		set_debugreg(current->thread.debugreg2, 2);
		set_debugreg(current->thread.debugreg3, 3);
		/* no 4 and 5 */
		set_debugreg(current->thread.debugreg6, 6);
		set_debugreg(current->thread.debugreg7, 7);
	}

}

static void __restore_processor_state(struct saved_context *ctxt)
{
	/*
	 * control registers
	 */
	/* cr4 was introduced in the Pentium CPU */
	if (ctxt->cr4)
		write_cr4(ctxt->cr4);
	write_cr3(ctxt->cr3);
	write_cr2(ctxt->cr2);
	write_cr0(ctxt->cr0);

	/*
	 * now restore the descriptor tables to their proper values
	 * ltr is done i fix_processor_context().
	 */
	load_gdt(&ctxt->gdt);
	load_idt(&ctxt->idt);

	/*
	 * segment registers
	 */
	loadsegment(es, ctxt->es);
	loadsegment(fs, ctxt->fs);
	loadsegment(gs, ctxt->gs);
	loadsegment(ss, ctxt->ss);

	/*
	 * sysenter MSRs
	 */
	if (boot_cpu_has(X86_FEATURE_SEP))
		enable_sep_cpu();

	/*
	 * restore XCR0 for xsave capable cpu's.
	 */
	if (cpu_has_xsave)
		xsetbv(XCR_XFEATURE_ENABLED_MASK, pcntxt_mask);

	fix_processor_context();
	do_fpu_end();
	mtrr_ap_init();
	mcheck_init(&boot_cpu_data);
}

/* Needed by apm.c */
void restore_processor_state(void)
{
	__restore_processor_state(&saved_context);
}
EXPORT_SYMBOL(restore_processor_state);

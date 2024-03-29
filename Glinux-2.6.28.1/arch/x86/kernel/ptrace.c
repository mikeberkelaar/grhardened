/* By Ross Biro 1/23/92 */
/*
 * Pentium III FXSR, SSE support
 *	Gareth Hughes <gareth@valinux.com>, May 2000
 *
 * BTS tracing
 *	Markus Metzger <markus.t.metzger@intel.com>, Dec 2007
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/errno.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/tracehook.h>
#include <linux/user.h>
#include <linux/elf.h>
#include <linux/security.h>
#include <linux/audit.h>
#include <linux/seccomp.h>
#include <linux/signal.h>

#include <asm/uaccess.h>
#include <asm/pgtable.h>
#include <asm/system.h>
#include <asm/processor.h>
#include <asm/i387.h>
#include <asm/debugreg.h>
#include <asm/ldt.h>
#include <asm/desc.h>
#include <asm/prctl.h>
#include <asm/proto.h>
#include <asm/ds.h>

#include "tls.h"

enum x86_regset {
	REGSET_GENERAL,
	REGSET_FP,
	REGSET_XFP,
	REGSET_IOPERM64 = REGSET_XFP,
	REGSET_TLS,
	REGSET_IOPERM32,
};

/*
 * does not yet catch signals sent when the child dies.
 * in exit.c or in signal.c.
 */

/*
 * Determines which flags the user has access to [1 = access, 0 = no access].
 */
#define FLAG_MASK_32		((unsigned long)			\
				 (X86_EFLAGS_CF | X86_EFLAGS_PF |	\
				  X86_EFLAGS_AF | X86_EFLAGS_ZF |	\
				  X86_EFLAGS_SF | X86_EFLAGS_TF |	\
				  X86_EFLAGS_DF | X86_EFLAGS_OF |	\
				  X86_EFLAGS_RF | X86_EFLAGS_AC))

/*
 * Determines whether a value may be installed in a segment register.
 */
static inline bool invalid_selector(u16 value)
{
	return unlikely(value != 0 && (value & SEGMENT_RPL_MASK) != USER_RPL);
}

#ifdef CONFIG_X86_32

#define FLAG_MASK		FLAG_MASK_32

static unsigned long *pt_regs_access(struct pt_regs *regs, unsigned long regno)
{
	BUILD_BUG_ON(offsetof(struct pt_regs, bx) != 0);
	regno >>= 2;
	if (regno > FS)
		--regno;
	return &regs->bx + regno;
}

static u16 get_segment_reg(struct task_struct *task, unsigned long offset)
{
	/*
	 * Returning the value truncates it to 16 bits.
	 */
	unsigned int retval;
	if (offset != offsetof(struct user_regs_struct, gs))
		retval = *pt_regs_access(task_pt_regs(task), offset);
	else {
		retval = task->thread.gs;
		if (task == current)
			savesegment(gs, retval);
	}
	return retval;
}

static int set_segment_reg(struct task_struct *task,
			   unsigned long offset, u16 value)
{
	/*
	 * The value argument was already truncated to 16 bits.
	 */
	if (invalid_selector(value))
		return -EIO;

	/*
	 * For %cs and %ss we cannot permit a null selector.
	 * We can permit a bogus selector as long as it has USER_RPL.
	 * Null selectors are fine for other segment registers, but
	 * we will never get back to user mode with invalid %cs or %ss
	 * and will take the trap in iret instead.  Much code relies
	 * on user_mode() to distinguish a user trap frame (which can
	 * safely use invalid selectors) from a kernel trap frame.
	 */
	switch (offset) {
	case offsetof(struct user_regs_struct, cs):
	case offsetof(struct user_regs_struct, ss):
		if (unlikely(value == 0))
			return -EIO;

	default:
		*pt_regs_access(task_pt_regs(task), offset) = value;
		break;

	case offsetof(struct user_regs_struct, gs):
		task->thread.gs = value;
		if (task == current)
			/*
			 * The user-mode %gs is not affected by
			 * kernel entry, so we must update the CPU.
			 */
			loadsegment(gs, value);
	}

	return 0;
}

static unsigned long debugreg_addr_limit(struct task_struct *task)
{
	return TASK_SIZE - 3;
}

#else  /* CONFIG_X86_64 */

#define FLAG_MASK		(FLAG_MASK_32 | X86_EFLAGS_NT)

static unsigned long *pt_regs_access(struct pt_regs *regs, unsigned long offset)
{
	BUILD_BUG_ON(offsetof(struct pt_regs, r15) != 0);
	return &regs->r15 + (offset / sizeof(regs->r15));
}

static u16 get_segment_reg(struct task_struct *task, unsigned long offset)
{
	/*
	 * Returning the value truncates it to 16 bits.
	 */
	unsigned int seg;

	switch (offset) {
	case offsetof(struct user_regs_struct, fs):
		if (task == current) {
			/* Older gas can't assemble movq %?s,%r?? */
			asm("movl %%fs,%0" : "=r" (seg));
			return seg;
		}
		return task->thread.fsindex;
	case offsetof(struct user_regs_struct, gs):
		if (task == current) {
			asm("movl %%gs,%0" : "=r" (seg));
			return seg;
		}
		return task->thread.gsindex;
	case offsetof(struct user_regs_struct, ds):
		if (task == current) {
			asm("movl %%ds,%0" : "=r" (seg));
			return seg;
		}
		return task->thread.ds;
	case offsetof(struct user_regs_struct, es):
		if (task == current) {
			asm("movl %%es,%0" : "=r" (seg));
			return seg;
		}
		return task->thread.es;

	case offsetof(struct user_regs_struct, cs):
	case offsetof(struct user_regs_struct, ss):
		break;
	}
	return *pt_regs_access(task_pt_regs(task), offset);
}

static int set_segment_reg(struct task_struct *task,
			   unsigned long offset, u16 value)
{
	/*
	 * The value argument was already truncated to 16 bits.
	 */
	if (invalid_selector(value))
		return -EIO;

	switch (offset) {
	case offsetof(struct user_regs_struct,fs):
		/*
		 * If this is setting fs as for normal 64-bit use but
		 * setting fs_base has implicitly changed it, leave it.
		 */
		if ((value == FS_TLS_SEL && task->thread.fsindex == 0 &&
		     task->thread.fs != 0) ||
		    (value == 0 && task->thread.fsindex == FS_TLS_SEL &&
		     task->thread.fs == 0))
			break;
		task->thread.fsindex = value;
		if (task == current)
			loadsegment(fs, task->thread.fsindex);
		break;
	case offsetof(struct user_regs_struct,gs):
		/*
		 * If this is setting gs as for normal 64-bit use but
		 * setting gs_base has implicitly changed it, leave it.
		 */
		if ((value == GS_TLS_SEL && task->thread.gsindex == 0 &&
		     task->thread.gs != 0) ||
		    (value == 0 && task->thread.gsindex == GS_TLS_SEL &&
		     task->thread.gs == 0))
			break;
		task->thread.gsindex = value;
		if (task == current)
			load_gs_index(task->thread.gsindex);
		break;
	case offsetof(struct user_regs_struct,ds):
		task->thread.ds = value;
		if (task == current)
			loadsegment(ds, task->thread.ds);
		break;
	case offsetof(struct user_regs_struct,es):
		task->thread.es = value;
		if (task == current)
			loadsegment(es, task->thread.es);
		break;

		/*
		 * Can't actually change these in 64-bit mode.
		 */
	case offsetof(struct user_regs_struct,cs):
		if (unlikely(value == 0))
			return -EIO;
#ifdef CONFIG_IA32_EMULATION
		if (test_tsk_thread_flag(task, TIF_IA32))
			task_pt_regs(task)->cs = value;
#endif
		break;
	case offsetof(struct user_regs_struct,ss):
		if (unlikely(value == 0))
			return -EIO;
#ifdef CONFIG_IA32_EMULATION
		if (test_tsk_thread_flag(task, TIF_IA32))
			task_pt_regs(task)->ss = value;
#endif
		break;
	}

	return 0;
}

static unsigned long debugreg_addr_limit(struct task_struct *task)
{
#ifdef CONFIG_IA32_EMULATION
	if (test_tsk_thread_flag(task, TIF_IA32))
		return IA32_PAGE_OFFSET - 3;
#endif
	return TASK_SIZE64 - 7;
}

#endif	/* CONFIG_X86_32 */

static unsigned long get_flags(struct task_struct *task)
{
	unsigned long retval = task_pt_regs(task)->flags;

	/*
	 * If the debugger set TF, hide it from the readout.
	 */
	if (test_tsk_thread_flag(task, TIF_FORCED_TF))
		retval &= ~X86_EFLAGS_TF;

	return retval;
}

static int set_flags(struct task_struct *task, unsigned long value)
{
	struct pt_regs *regs = task_pt_regs(task);

	/*
	 * If the user value contains TF, mark that
	 * it was not "us" (the debugger) that set it.
	 * If not, make sure it stays set if we had.
	 */
	if (value & X86_EFLAGS_TF)
		clear_tsk_thread_flag(task, TIF_FORCED_TF);
	else if (test_tsk_thread_flag(task, TIF_FORCED_TF))
		value |= X86_EFLAGS_TF;

	regs->flags = (regs->flags & ~FLAG_MASK) | (value & FLAG_MASK);

	return 0;
}

static int putreg(struct task_struct *child,
		  unsigned long offset, unsigned long value)
{
	switch (offset) {
	case offsetof(struct user_regs_struct, cs):
	case offsetof(struct user_regs_struct, ds):
	case offsetof(struct user_regs_struct, es):
	case offsetof(struct user_regs_struct, fs):
	case offsetof(struct user_regs_struct, gs):
	case offsetof(struct user_regs_struct, ss):
		return set_segment_reg(child, offset, value);

	case offsetof(struct user_regs_struct, flags):
		return set_flags(child, value);

#ifdef CONFIG_X86_64
	/*
	 * Orig_ax is really just a flag with small positive and
	 * negative values, so make sure to always sign-extend it
	 * from 32 bits so that it works correctly regardless of
	 * whether we come from a 32-bit environment or not.
	 */
	case offsetof(struct user_regs_struct, orig_ax):
		value = (long) (s32) value;
		break;

	case offsetof(struct user_regs_struct,fs_base):
		if (value >= TASK_SIZE_OF(child))
			return -EIO;
		/*
		 * When changing the segment base, use do_arch_prctl
		 * to set either thread.fs or thread.fsindex and the
		 * corresponding GDT slot.
		 */
		if (child->thread.fs != value)
			return do_arch_prctl(child, ARCH_SET_FS, value);
		return 0;
	case offsetof(struct user_regs_struct,gs_base):
		/*
		 * Exactly the same here as the %fs handling above.
		 */
		if (value >= TASK_SIZE_OF(child))
			return -EIO;
		if (child->thread.gs != value)
			return do_arch_prctl(child, ARCH_SET_GS, value);
		return 0;
#endif
	}

	*pt_regs_access(task_pt_regs(child), offset) = value;
	return 0;
}

static unsigned long getreg(struct task_struct *task, unsigned long offset)
{
	switch (offset) {
	case offsetof(struct user_regs_struct, cs):
	case offsetof(struct user_regs_struct, ds):
	case offsetof(struct user_regs_struct, es):
	case offsetof(struct user_regs_struct, fs):
	case offsetof(struct user_regs_struct, gs):
	case offsetof(struct user_regs_struct, ss):
		return get_segment_reg(task, offset);

	case offsetof(struct user_regs_struct, flags):
		return get_flags(task);

#ifdef CONFIG_X86_64
	case offsetof(struct user_regs_struct, fs_base): {
		/*
		 * do_arch_prctl may have used a GDT slot instead of
		 * the MSR.  To userland, it appears the same either
		 * way, except the %fs segment selector might not be 0.
		 */
		unsigned int seg = task->thread.fsindex;
		if (task->thread.fs != 0)
			return task->thread.fs;
		if (task == current)
			asm("movl %%fs,%0" : "=r" (seg));
		if (seg != FS_TLS_SEL)
			return 0;
		return get_desc_base(&task->thread.tls_array[FS_TLS]);
	}
	case offsetof(struct user_regs_struct, gs_base): {
		/*
		 * Exactly the same here as the %fs handling above.
		 */
		unsigned int seg = task->thread.gsindex;
		if (task->thread.gs != 0)
			return task->thread.gs;
		if (task == current)
			asm("movl %%gs,%0" : "=r" (seg));
		if (seg != GS_TLS_SEL)
			return 0;
		return get_desc_base(&task->thread.tls_array[GS_TLS]);
	}
#endif
	}

	return *pt_regs_access(task_pt_regs(task), offset);
}

static int genregs_get(struct task_struct *target,
		       const struct user_regset *regset,
		       unsigned int pos, unsigned int count,
		       void *kbuf, void __user *ubuf)
{
	if (kbuf) {
		unsigned long *k = kbuf;
		while (count > 0) {
			*k++ = getreg(target, pos);
			count -= sizeof(*k);
			pos += sizeof(*k);
		}
	} else {
		unsigned long __user *u = ubuf;
		while (count > 0) {
			if (__put_user(getreg(target, pos), u++))
				return -EFAULT;
			count -= sizeof(*u);
			pos += sizeof(*u);
		}
	}

	return 0;
}

static int genregs_set(struct task_struct *target,
		       const struct user_regset *regset,
		       unsigned int pos, unsigned int count,
		       const void *kbuf, const void __user *ubuf)
{
	int ret = 0;
	if (kbuf) {
		const unsigned long *k = kbuf;
		while (count > 0 && !ret) {
			ret = putreg(target, pos, *k++);
			count -= sizeof(*k);
			pos += sizeof(*k);
		}
	} else {
		const unsigned long  __user *u = ubuf;
		while (count > 0 && !ret) {
			unsigned long word;
			ret = __get_user(word, u++);
			if (ret)
				break;
			ret = putreg(target, pos, word);
			count -= sizeof(*u);
			pos += sizeof(*u);
		}
	}
	return ret;
}

/*
 * This function is trivial and will be inlined by the compiler.
 * Having it separates the implementation details of debug
 * registers from the interface details of ptrace.
 */
static unsigned long ptrace_get_debugreg(struct task_struct *child, int n)
{
	switch (n) {
	case 0:		return child->thread.debugreg0;
	case 1:		return child->thread.debugreg1;
	case 2:		return child->thread.debugreg2;
	case 3:		return child->thread.debugreg3;
	case 6:		return child->thread.debugreg6;
	case 7:		return child->thread.debugreg7;
	}
	return 0;
}

static int ptrace_set_debugreg(struct task_struct *child,
			       int n, unsigned long data)
{
	int i;

	if (unlikely(n == 4 || n == 5))
		return -EIO;

	if (n < 4 && unlikely(data >= debugreg_addr_limit(child)))
		return -EIO;

	switch (n) {
	case 0:		child->thread.debugreg0 = data; break;
	case 1:		child->thread.debugreg1 = data; break;
	case 2:		child->thread.debugreg2 = data; break;
	case 3:		child->thread.debugreg3 = data; break;

	case 6:
		if ((data & ~0xffffffffUL) != 0)
			return -EIO;
		child->thread.debugreg6 = data;
		break;

	case 7:
		/*
		 * Sanity-check data. Take one half-byte at once with
		 * check = (val >> (16 + 4*i)) & 0xf. It contains the
		 * R/Wi and LENi bits; bits 0 and 1 are R/Wi, and bits
		 * 2 and 3 are LENi. Given a list of invalid values,
		 * we do mask |= 1 << invalid_value, so that
		 * (mask >> check) & 1 is a correct test for invalid
		 * values.
		 *
		 * R/Wi contains the type of the breakpoint /
		 * watchpoint, LENi contains the length of the watched
		 * data in the watchpoint case.
		 *
		 * The invalid values are:
		 * - LENi == 0x10 (undefined), so mask |= 0x0f00.	[32-bit]
		 * - R/Wi == 0x10 (break on I/O reads or writes), so
		 *   mask |= 0x4444.
		 * - R/Wi == 0x00 && LENi != 0x00, so we have mask |=
		 *   0x1110.
		 *
		 * Finally, mask = 0x0f00 | 0x4444 | 0x1110 == 0x5f54.
		 *
		 * See the Intel Manual "System Programming Guide",
		 * 15.2.4
		 *
		 * Note that LENi == 0x10 is defined on x86_64 in long
		 * mode (i.e. even for 32-bit userspace software, but
		 * 64-bit kernel), so the x86_64 mask value is 0x5454.
		 * See the AMD manual no. 24593 (AMD64 System Programming)
		 */
#ifdef CONFIG_X86_32
#define	DR7_MASK	0x5f54
#else
#define	DR7_MASK	0x5554
#endif
		data &= ~DR_CONTROL_RESERVED;
		for (i = 0; i < 4; i++)
			if ((DR7_MASK >> ((data >> (16 + 4*i)) & 0xf)) & 1)
				return -EIO;
		child->thread.debugreg7 = data;
		if (data)
			set_tsk_thread_flag(child, TIF_DEBUG);
		else
			clear_tsk_thread_flag(child, TIF_DEBUG);
		break;
	}

	return 0;
}

/*
 * These access the current or another (stopped) task's io permission
 * bitmap for debugging or core dump.
 */
static int ioperm_active(struct task_struct *target,
			 const struct user_regset *regset)
{
	return target->thread.io_bitmap_max / regset->size;
}

static int ioperm_get(struct task_struct *target,
		      const struct user_regset *regset,
		      unsigned int pos, unsigned int count,
		      void *kbuf, void __user *ubuf)
{
	if (!target->thread.io_bitmap_ptr)
		return -ENXIO;

	return user_regset_copyout(&pos, &count, &kbuf, &ubuf,
				   target->thread.io_bitmap_ptr,
				   0, IO_BITMAP_BYTES);
}

#ifdef CONFIG_X86_PTRACE_BTS
/*
 * The configuration for a particular BTS hardware implementation.
 */
struct bts_configuration {
	/* the size of a BTS record in bytes; at most BTS_MAX_RECORD_SIZE */
	unsigned char  sizeof_bts;
	/* the size of a field in the BTS record in bytes */
	unsigned char  sizeof_field;
	/* a bitmask to enable/disable BTS in DEBUGCTL MSR */
	unsigned long debugctl_mask;
};
static struct bts_configuration bts_cfg;

#define BTS_MAX_RECORD_SIZE (8 * 3)


/*
 * Branch Trace Store (BTS) uses the following format. Different
 * architectures vary in the size of those fields.
 * - source linear address
 * - destination linear address
 * - flags
 *
 * Later architectures use 64bit pointers throughout, whereas earlier
 * architectures use 32bit pointers in 32bit mode.
 *
 * We compute the base address for the first 8 fields based on:
 * - the field size stored in the DS configuration
 * - the relative field position
 *
 * In order to store additional information in the BTS buffer, we use
 * a special source address to indicate that the record requires
 * special interpretation.
 *
 * Netburst indicated via a bit in the flags field whether the branch
 * was predicted; this is ignored.
 */

enum bts_field {
	bts_from = 0,
	bts_to,
	bts_flags,

	bts_escape = (unsigned long)-1,
	bts_qual = bts_to,
	bts_jiffies = bts_flags
};

static inline unsigned long bts_get(const char *base, enum bts_field field)
{
	base += (bts_cfg.sizeof_field * field);
	return *(unsigned long *)base;
}

static inline void bts_set(char *base, enum bts_field field, unsigned long val)
{
	base += (bts_cfg.sizeof_field * field);;
	(*(unsigned long *)base) = val;
}

/*
 * Translate a BTS record from the raw format into the bts_struct format
 *
 * out (out): bts_struct interpretation
 * raw: raw BTS record
 */
static void ptrace_bts_translate_record(struct bts_struct *out, const void *raw)
{
	memset(out, 0, sizeof(*out));
	if (bts_get(raw, bts_from) == bts_escape) {
		out->qualifier       = bts_get(raw, bts_qual);
		out->variant.jiffies = bts_get(raw, bts_jiffies);
	} else {
		out->qualifier = BTS_BRANCH;
		out->variant.lbr.from_ip = bts_get(raw, bts_from);
		out->variant.lbr.to_ip   = bts_get(raw, bts_to);
	}
}

static int ptrace_bts_read_record(struct task_struct *child, size_t index,
				  struct bts_struct __user *out)
{
	struct bts_struct ret;
	const void *bts_record;
	size_t bts_index, bts_end;
	int error;

	error = ds_get_bts_end(child, &bts_end);
	if (error < 0)
		return error;

	if (bts_end <= index)
		return -EINVAL;

	error = ds_get_bts_index(child, &bts_index);
	if (error < 0)
		return error;

	/* translate the ptrace bts index into the ds bts index */
	bts_index += bts_end - (index + 1);
	if (bts_end <= bts_index)
		bts_index -= bts_end;

	error = ds_access_bts(child, bts_index, &bts_record);
	if (error < 0)
		return error;

	ptrace_bts_translate_record(&ret, bts_record);

	if (copy_to_user(out, &ret, sizeof(ret)))
		return -EFAULT;

	return sizeof(ret);
}

static int ptrace_bts_drain(struct task_struct *child,
			    long size,
			    struct bts_struct __user *out)
{
	struct bts_struct ret;
	const unsigned char *raw;
	size_t end, i;
	int error;

	error = ds_get_bts_index(child, &end);
	if (error < 0)
		return error;

	if (size < (end * sizeof(struct bts_struct)))
		return -EIO;

	error = ds_access_bts(child, 0, (const void **)&raw);
	if (error < 0)
		return error;

	for (i = 0; i < end; i++, out++, raw += bts_cfg.sizeof_bts) {
		ptrace_bts_translate_record(&ret, raw);

		if (copy_to_user(out, &ret, sizeof(ret)))
			return -EFAULT;
	}

	error = ds_clear_bts(child);
	if (error < 0)
		return error;

	return end;
}

static void ptrace_bts_ovfl(struct task_struct *child)
{
	send_sig(child->thread.bts_ovfl_signal, child, 0);
}

static int ptrace_bts_config(struct task_struct *child,
			     long cfg_size,
			     const struct ptrace_bts_config __user *ucfg)
{
	struct ptrace_bts_config cfg;
	int error = 0;

	error = -EOPNOTSUPP;
	if (!bts_cfg.sizeof_bts)
		goto errout;

	error = -EIO;
	if (cfg_size < sizeof(cfg))
		goto errout;

	error = -EFAULT;
	if (copy_from_user(&cfg, ucfg, sizeof(cfg)))
		goto errout;

	error = -EINVAL;
	if ((cfg.flags & PTRACE_BTS_O_SIGNAL) &&
	    !(cfg.flags & PTRACE_BTS_O_ALLOC))
		goto errout;

	if (cfg.flags & PTRACE_BTS_O_ALLOC) {
		ds_ovfl_callback_t ovfl = NULL;
		unsigned int sig = 0;

		/* we ignore the error in case we were not tracing child */
		(void)ds_release_bts(child);

		if (cfg.flags & PTRACE_BTS_O_SIGNAL) {
			if (!cfg.signal)
				goto errout;

			sig  = cfg.signal;
			ovfl = ptrace_bts_ovfl;
		}

		error = ds_request_bts(child, /* base = */ NULL, cfg.size, ovfl);
		if (error < 0)
			goto errout;

		child->thread.bts_ovfl_signal = sig;
	}

	error = -EINVAL;
	if (!child->thread.ds_ctx && cfg.flags)
		goto errout;

	if (cfg.flags & PTRACE_BTS_O_TRACE)
		child->thread.debugctlmsr |= bts_cfg.debugctl_mask;
	else
		child->thread.debugctlmsr &= ~bts_cfg.debugctl_mask;

	if (cfg.flags & PTRACE_BTS_O_SCHED)
		set_tsk_thread_flag(child, TIF_BTS_TRACE_TS);
	else
		clear_tsk_thread_flag(child, TIF_BTS_TRACE_TS);

	error = sizeof(cfg);

out:
	if (child->thread.debugctlmsr)
		set_tsk_thread_flag(child, TIF_DEBUGCTLMSR);
	else
		clear_tsk_thread_flag(child, TIF_DEBUGCTLMSR);

	return error;

errout:
	child->thread.debugctlmsr &= ~bts_cfg.debugctl_mask;
	clear_tsk_thread_flag(child, TIF_BTS_TRACE_TS);
	goto out;
}

static int ptrace_bts_status(struct task_struct *child,
			     long cfg_size,
			     struct ptrace_bts_config __user *ucfg)
{
	struct ptrace_bts_config cfg;
	size_t end;
	const void *base, *max;
	int error;

	if (cfg_size < sizeof(cfg))
		return -EIO;

	error = ds_get_bts_end(child, &end);
	if (error < 0)
		return error;

	error = ds_access_bts(child, /* index = */ 0, &base);
	if (error < 0)
		return error;

	error = ds_access_bts(child, /* index = */ end, &max);
	if (error < 0)
		return error;

	memset(&cfg, 0, sizeof(cfg));
	cfg.size = (max - base);
	cfg.signal = child->thread.bts_ovfl_signal;
	cfg.bts_size = sizeof(struct bts_struct);

	if (cfg.signal)
		cfg.flags |= PTRACE_BTS_O_SIGNAL;

	if (test_tsk_thread_flag(child, TIF_DEBUGCTLMSR) &&
	    child->thread.debugctlmsr & bts_cfg.debugctl_mask)
		cfg.flags |= PTRACE_BTS_O_TRACE;

	if (test_tsk_thread_flag(child, TIF_BTS_TRACE_TS))
		cfg.flags |= PTRACE_BTS_O_SCHED;

	if (copy_to_user(ucfg, &cfg, sizeof(cfg)))
		return -EFAULT;

	return sizeof(cfg);
}

static int ptrace_bts_write_record(struct task_struct *child,
				   const struct bts_struct *in)
{
	unsigned char bts_record[BTS_MAX_RECORD_SIZE];

	BUG_ON(BTS_MAX_RECORD_SIZE < bts_cfg.sizeof_bts);

	memset(bts_record, 0, bts_cfg.sizeof_bts);
	switch (in->qualifier) {
	case BTS_INVALID:
		break;

	case BTS_BRANCH:
		bts_set(bts_record, bts_from, in->variant.lbr.from_ip);
		bts_set(bts_record, bts_to,   in->variant.lbr.to_ip);
		break;

	case BTS_TASK_ARRIVES:
	case BTS_TASK_DEPARTS:
		bts_set(bts_record, bts_from,    bts_escape);
		bts_set(bts_record, bts_qual,    in->qualifier);
		bts_set(bts_record, bts_jiffies, in->variant.jiffies);
		break;

	default:
		return -EINVAL;
	}

	/* The writing task will be the switched-to task on a context
	 * switch. It needs to write into the switched-from task's BTS
	 * buffer. */
	return ds_unchecked_write_bts(child, bts_record, bts_cfg.sizeof_bts);
}

void ptrace_bts_take_timestamp(struct task_struct *tsk,
			       enum bts_qualifier qualifier)
{
	struct bts_struct rec = {
		.qualifier = qualifier,
		.variant.jiffies = jiffies_64
	};

	ptrace_bts_write_record(tsk, &rec);
}

static const struct bts_configuration bts_cfg_netburst = {
	.sizeof_bts    = sizeof(long) * 3,
	.sizeof_field  = sizeof(long),
	.debugctl_mask = (1<<2)|(1<<3)|(1<<5)
};

static const struct bts_configuration bts_cfg_pentium_m = {
	.sizeof_bts    = sizeof(long) * 3,
	.sizeof_field  = sizeof(long),
	.debugctl_mask = (1<<6)|(1<<7)
};

static const struct bts_configuration bts_cfg_core2 = {
	.sizeof_bts    = 8 * 3,
	.sizeof_field  = 8,
	.debugctl_mask = (1<<6)|(1<<7)|(1<<9)
};

static inline void bts_configure(const struct bts_configuration *cfg)
{
	bts_cfg = *cfg;
}

void __cpuinit ptrace_bts_init_intel(struct cpuinfo_x86 *c)
{
	switch (c->x86) {
	case 0x6:
		switch (c->x86_model) {
		case 0xD:
		case 0xE: /* Pentium M */
			bts_configure(&bts_cfg_pentium_m);
			break;
		case 0xF: /* Core2 */
        case 0x1C: /* Atom */
			bts_configure(&bts_cfg_core2);
			break;
		default:
			/* sorry, don't know about them */
			break;
		}
		break;
	case 0xF:
		switch (c->x86_model) {
		case 0x0:
		case 0x1:
		case 0x2: /* Netburst */
			bts_configure(&bts_cfg_netburst);
			break;
		default:
			/* sorry, don't know about them */
			break;
		}
		break;
	default:
		/* sorry, don't know about them */
		break;
	}
}
#endif /* CONFIG_X86_PTRACE_BTS */

/*
 * Called by kernel/ptrace.c when detaching..
 *
 * Make sure the single step bit is not set.
 */
void ptrace_disable(struct task_struct *child)
{
	user_disable_single_step(child);
#ifdef TIF_SYSCALL_EMU
	clear_tsk_thread_flag(child, TIF_SYSCALL_EMU);
#endif
#ifdef CONFIG_X86_PTRACE_BTS
	(void)ds_release_bts(child);

	child->thread.debugctlmsr &= ~bts_cfg.debugctl_mask;
	if (!child->thread.debugctlmsr)
		clear_tsk_thread_flag(child, TIF_DEBUGCTLMSR);

	clear_tsk_thread_flag(child, TIF_BTS_TRACE_TS);
#endif /* CONFIG_X86_PTRACE_BTS */
}

#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
static const struct user_regset_view user_x86_32_view; /* Initialized below. */
#endif

long arch_ptrace(struct task_struct *child, long request, long addr, long data)
{
	int ret;
	unsigned long __user *datap = (unsigned long __user *)data;

	switch (request) {
	/* read the word at location addr in the USER area. */
	case PTRACE_PEEKUSR: {
		unsigned long tmp;

		ret = -EIO;
		if ((addr & (sizeof(data) - 1)) || addr < 0 ||
		    addr >= sizeof(struct user))
			break;

		tmp = 0;  /* Default return condition */
		if (addr < sizeof(struct user_regs_struct))
			tmp = getreg(child, addr);
		else if (addr >= offsetof(struct user, u_debugreg[0]) &&
			 addr <= offsetof(struct user, u_debugreg[7])) {
			addr -= offsetof(struct user, u_debugreg[0]);
			tmp = ptrace_get_debugreg(child, addr / sizeof(data));
		}
		ret = put_user(tmp, datap);
		break;
	}

	case PTRACE_POKEUSR: /* write the word at location addr in the USER area */
		ret = -EIO;
		if ((addr & (sizeof(data) - 1)) || addr < 0 ||
		    addr >= sizeof(struct user))
			break;

		if (addr < sizeof(struct user_regs_struct))
			ret = putreg(child, addr, data);
		else if (addr >= offsetof(struct user, u_debugreg[0]) &&
			 addr <= offsetof(struct user, u_debugreg[7])) {
			addr -= offsetof(struct user, u_debugreg[0]);
			ret = ptrace_set_debugreg(child,
						  addr / sizeof(data), data);
		}
		break;

	case PTRACE_GETREGS:	/* Get all gp regs from the child. */
		return copy_regset_to_user(child,
					   task_user_regset_view(current),
					   REGSET_GENERAL,
					   0, sizeof(struct user_regs_struct),
					   datap);

	case PTRACE_SETREGS:	/* Set all gp regs in the child. */
		return copy_regset_from_user(child,
					     task_user_regset_view(current),
					     REGSET_GENERAL,
					     0, sizeof(struct user_regs_struct),
					     datap);

	case PTRACE_GETFPREGS:	/* Get the child FPU state. */
		return copy_regset_to_user(child,
					   task_user_regset_view(current),
					   REGSET_FP,
					   0, sizeof(struct user_i387_struct),
					   datap);

	case PTRACE_SETFPREGS:	/* Set the child FPU state. */
		return copy_regset_from_user(child,
					     task_user_regset_view(current),
					     REGSET_FP,
					     0, sizeof(struct user_i387_struct),
					     datap);

#ifdef CONFIG_X86_32
	case PTRACE_GETFPXREGS:	/* Get the child extended FPU state. */
		return copy_regset_to_user(child, &user_x86_32_view,
					   REGSET_XFP,
					   0, sizeof(struct user_fxsr_struct),
					   datap) ? -EIO : 0;

	case PTRACE_SETFPXREGS:	/* Set the child extended FPU state. */
		return copy_regset_from_user(child, &user_x86_32_view,
					     REGSET_XFP,
					     0, sizeof(struct user_fxsr_struct),
					     datap) ? -EIO : 0;
#endif

#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
	case PTRACE_GET_THREAD_AREA:
		if (addr < 0)
			return -EIO;
		ret = do_get_thread_area(child, addr,
					 (struct user_desc __user *) data);
		break;

	case PTRACE_SET_THREAD_AREA:
		if (addr < 0)
			return -EIO;
		ret = do_set_thread_area(child, addr,
					 (struct user_desc __user *) data, 0);
		break;
#endif

#ifdef CONFIG_X86_64
		/* normal 64bit interface to access TLS data.
		   Works just like arch_prctl, except that the arguments
		   are reversed. */
	case PTRACE_ARCH_PRCTL:
		ret = do_arch_prctl(child, data, addr);
		break;
#endif

	/*
	 * These bits need more cooking - not enabled yet:
	 */
#ifdef CONFIG_X86_PTRACE_BTS
	case PTRACE_BTS_CONFIG:
		ret = ptrace_bts_config
			(child, data, (struct ptrace_bts_config __user *)addr);
		break;

	case PTRACE_BTS_STATUS:
		ret = ptrace_bts_status
			(child, data, (struct ptrace_bts_config __user *)addr);
		break;

	case PTRACE_BTS_SIZE:
		ret = ds_get_bts_index(child, /* pos = */ NULL);
		break;

	case PTRACE_BTS_GET:
		ret = ptrace_bts_read_record
			(child, data, (struct bts_struct __user *) addr);
		break;

	case PTRACE_BTS_CLEAR:
		ret = ds_clear_bts(child);
		break;

	case PTRACE_BTS_DRAIN:
		ret = ptrace_bts_drain
			(child, data, (struct bts_struct __user *) addr);
		break;
#endif /* CONFIG_X86_PTRACE_BTS */

	default:
		ret = ptrace_request(child, request, addr, data);
		break;
	}

	return ret;
}

#ifdef CONFIG_IA32_EMULATION

#include <linux/compat.h>
#include <linux/syscalls.h>
#include <asm/ia32.h>
#include <asm/user32.h>

#define R32(l,q)							\
	case offsetof(struct user32, regs.l):				\
		regs->q = value; break

#define SEG32(rs)							\
	case offsetof(struct user32, regs.rs):				\
		return set_segment_reg(child,				\
				       offsetof(struct user_regs_struct, rs), \
				       value);				\
		break

static int putreg32(struct task_struct *child, unsigned regno, u32 value)
{
	struct pt_regs *regs = task_pt_regs(child);

	switch (regno) {

	SEG32(cs);
	SEG32(ds);
	SEG32(es);
	SEG32(fs);
	SEG32(gs);
	SEG32(ss);

	R32(ebx, bx);
	R32(ecx, cx);
	R32(edx, dx);
	R32(edi, di);
	R32(esi, si);
	R32(ebp, bp);
	R32(eax, ax);
	R32(eip, ip);
	R32(esp, sp);

	case offsetof(struct user32, regs.orig_eax):
		/*
		 * Sign-extend the value so that orig_eax = -1
		 * causes (long)orig_ax < 0 tests to fire correctly.
		 */
		regs->orig_ax = (long) (s32) value;
		break;

	case offsetof(struct user32, regs.eflags):
		return set_flags(child, value);

	case offsetof(struct user32, u_debugreg[0]) ...
		offsetof(struct user32, u_debugreg[7]):
		regno -= offsetof(struct user32, u_debugreg[0]);
		return ptrace_set_debugreg(child, regno / 4, value);

	default:
		if (regno > sizeof(struct user32) || (regno & 3))
			return -EIO;

		/*
		 * Other dummy fields in the virtual user structure
		 * are ignored
		 */
		break;
	}
	return 0;
}

#undef R32
#undef SEG32

#define R32(l,q)							\
	case offsetof(struct user32, regs.l):				\
		*val = regs->q; break

#define SEG32(rs)							\
	case offsetof(struct user32, regs.rs):				\
		*val = get_segment_reg(child,				\
				       offsetof(struct user_regs_struct, rs)); \
		break

static int getreg32(struct task_struct *child, unsigned regno, u32 *val)
{
	struct pt_regs *regs = task_pt_regs(child);

	switch (regno) {

	SEG32(ds);
	SEG32(es);
	SEG32(fs);
	SEG32(gs);

	R32(cs, cs);
	R32(ss, ss);
	R32(ebx, bx);
	R32(ecx, cx);
	R32(edx, dx);
	R32(edi, di);
	R32(esi, si);
	R32(ebp, bp);
	R32(eax, ax);
	R32(orig_eax, orig_ax);
	R32(eip, ip);
	R32(esp, sp);

	case offsetof(struct user32, regs.eflags):
		*val = get_flags(child);
		break;

	case offsetof(struct user32, u_debugreg[0]) ...
		offsetof(struct user32, u_debugreg[7]):
		regno -= offsetof(struct user32, u_debugreg[0]);
		*val = ptrace_get_debugreg(child, regno / 4);
		break;

	default:
		if (regno > sizeof(struct user32) || (regno & 3))
			return -EIO;

		/*
		 * Other dummy fields in the virtual user structure
		 * are ignored
		 */
		*val = 0;
		break;
	}
	return 0;
}

#undef R32
#undef SEG32

static int genregs32_get(struct task_struct *target,
			 const struct user_regset *regset,
			 unsigned int pos, unsigned int count,
			 void *kbuf, void __user *ubuf)
{
	if (kbuf) {
		compat_ulong_t *k = kbuf;
		while (count > 0) {
			getreg32(target, pos, k++);
			count -= sizeof(*k);
			pos += sizeof(*k);
		}
	} else {
		compat_ulong_t __user *u = ubuf;
		while (count > 0) {
			compat_ulong_t word;
			getreg32(target, pos, &word);
			if (__put_user(word, u++))
				return -EFAULT;
			count -= sizeof(*u);
			pos += sizeof(*u);
		}
	}

	return 0;
}

static int genregs32_set(struct task_struct *target,
			 const struct user_regset *regset,
			 unsigned int pos, unsigned int count,
			 const void *kbuf, const void __user *ubuf)
{
	int ret = 0;
	if (kbuf) {
		const compat_ulong_t *k = kbuf;
		while (count > 0 && !ret) {
			ret = putreg32(target, pos, *k++);
			count -= sizeof(*k);
			pos += sizeof(*k);
		}
	} else {
		const compat_ulong_t __user *u = ubuf;
		while (count > 0 && !ret) {
			compat_ulong_t word;
			ret = __get_user(word, u++);
			if (ret)
				break;
			ret = putreg32(target, pos, word);
			count -= sizeof(*u);
			pos += sizeof(*u);
		}
	}
	return ret;
}

long compat_arch_ptrace(struct task_struct *child, compat_long_t request,
			compat_ulong_t caddr, compat_ulong_t cdata)
{
	unsigned long addr = caddr;
	unsigned long data = cdata;
	void __user *datap = compat_ptr(data);
	int ret;
	__u32 val;

	switch (request) {
	case PTRACE_PEEKUSR:
		ret = getreg32(child, addr, &val);
		if (ret == 0)
			ret = put_user(val, (__u32 __user *)datap);
		break;

	case PTRACE_POKEUSR:
		ret = putreg32(child, addr, data);
		break;

	case PTRACE_GETREGS:	/* Get all gp regs from the child. */
		return copy_regset_to_user(child, &user_x86_32_view,
					   REGSET_GENERAL,
					   0, sizeof(struct user_regs_struct32),
					   datap);

	case PTRACE_SETREGS:	/* Set all gp regs in the child. */
		return copy_regset_from_user(child, &user_x86_32_view,
					     REGSET_GENERAL, 0,
					     sizeof(struct user_regs_struct32),
					     datap);

	case PTRACE_GETFPREGS:	/* Get the child FPU state. */
		return copy_regset_to_user(child, &user_x86_32_view,
					   REGSET_FP, 0,
					   sizeof(struct user_i387_ia32_struct),
					   datap);

	case PTRACE_SETFPREGS:	/* Set the child FPU state. */
		return copy_regset_from_user(
			child, &user_x86_32_view, REGSET_FP,
			0, sizeof(struct user_i387_ia32_struct), datap);

	case PTRACE_GETFPXREGS:	/* Get the child extended FPU state. */
		return copy_regset_to_user(child, &user_x86_32_view,
					   REGSET_XFP, 0,
					   sizeof(struct user32_fxsr_struct),
					   datap);

	case PTRACE_SETFPXREGS:	/* Set the child extended FPU state. */
		return copy_regset_from_user(child, &user_x86_32_view,
					     REGSET_XFP, 0,
					     sizeof(struct user32_fxsr_struct),
					     datap);

	case PTRACE_GET_THREAD_AREA:
	case PTRACE_SET_THREAD_AREA:
		return arch_ptrace(child, request, addr, data);

	default:
		return compat_ptrace_request(child, request, addr, data);
	}

	return ret;
}

#endif	/* CONFIG_IA32_EMULATION */

#ifdef CONFIG_X86_64

static const struct user_regset x86_64_regsets[] = {
	[REGSET_GENERAL] = {
		.core_note_type = NT_PRSTATUS,
		.n = sizeof(struct user_regs_struct) / sizeof(long),
		.size = sizeof(long), .align = sizeof(long),
		.get = genregs_get, .set = genregs_set
	},
	[REGSET_FP] = {
		.core_note_type = NT_PRFPREG,
		.n = sizeof(struct user_i387_struct) / sizeof(long),
		.size = sizeof(long), .align = sizeof(long),
		.active = xfpregs_active, .get = xfpregs_get, .set = xfpregs_set
	},
	[REGSET_IOPERM64] = {
		.core_note_type = NT_386_IOPERM,
		.n = IO_BITMAP_LONGS,
		.size = sizeof(long), .align = sizeof(long),
		.active = ioperm_active, .get = ioperm_get
	},
};

static const struct user_regset_view user_x86_64_view = {
	.name = "x86_64", .e_machine = EM_X86_64,
	.regsets = x86_64_regsets, .n = ARRAY_SIZE(x86_64_regsets)
};

#else  /* CONFIG_X86_32 */

#define user_regs_struct32	user_regs_struct
#define genregs32_get		genregs_get
#define genregs32_set		genregs_set

#define user_i387_ia32_struct	user_i387_struct
#define user32_fxsr_struct	user_fxsr_struct

#endif	/* CONFIG_X86_64 */

#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
static const struct user_regset x86_32_regsets[] = {
	[REGSET_GENERAL] = {
		.core_note_type = NT_PRSTATUS,
		.n = sizeof(struct user_regs_struct32) / sizeof(u32),
		.size = sizeof(u32), .align = sizeof(u32),
		.get = genregs32_get, .set = genregs32_set
	},
	[REGSET_FP] = {
		.core_note_type = NT_PRFPREG,
		.n = sizeof(struct user_i387_ia32_struct) / sizeof(u32),
		.size = sizeof(u32), .align = sizeof(u32),
		.active = fpregs_active, .get = fpregs_get, .set = fpregs_set
	},
	[REGSET_XFP] = {
		.core_note_type = NT_PRXFPREG,
		.n = sizeof(struct user32_fxsr_struct) / sizeof(u32),
		.size = sizeof(u32), .align = sizeof(u32),
		.active = xfpregs_active, .get = xfpregs_get, .set = xfpregs_set
	},
	[REGSET_TLS] = {
		.core_note_type = NT_386_TLS,
		.n = GDT_ENTRY_TLS_ENTRIES, .bias = GDT_ENTRY_TLS_MIN,
		.size = sizeof(struct user_desc),
		.align = sizeof(struct user_desc),
		.active = regset_tls_active,
		.get = regset_tls_get, .set = regset_tls_set
	},
	[REGSET_IOPERM32] = {
		.core_note_type = NT_386_IOPERM,
		.n = IO_BITMAP_BYTES / sizeof(u32),
		.size = sizeof(u32), .align = sizeof(u32),
		.active = ioperm_active, .get = ioperm_get
	},
};

static const struct user_regset_view user_x86_32_view = {
	.name = "i386", .e_machine = EM_386,
	.regsets = x86_32_regsets, .n = ARRAY_SIZE(x86_32_regsets)
};
#endif

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
#ifdef CONFIG_IA32_EMULATION
	if (test_tsk_thread_flag(task, TIF_IA32))
#endif
#if defined CONFIG_X86_32 || defined CONFIG_IA32_EMULATION
		return &user_x86_32_view;
#endif
#ifdef CONFIG_X86_64
	return &user_x86_64_view;
#endif
}

void send_sigtrap(struct task_struct *tsk, struct pt_regs *regs,
					 int error_code, int si_code)
{
	struct siginfo info;

	tsk->thread.trap_no = 1;
	tsk->thread.error_code = error_code;

	memset(&info, 0, sizeof(info));
	info.si_signo = SIGTRAP;
	info.si_code = si_code;

	/* User-mode ip? */
	info.si_addr = user_mode(regs) ? (void __user *) regs->ip : NULL;

	/* Send us the fake SIGTRAP */
	force_sig_info(SIGTRAP, &info, tsk);
}


#ifdef CONFIG_X86_32
# define IS_IA32	1
#elif defined CONFIG_IA32_EMULATION
# define IS_IA32	test_thread_flag(TIF_IA32)
#else
# define IS_IA32	0
#endif

/*
 * We must return the syscall number to actually look up in the table.
 * This can be -1L to skip running any syscall at all.
 */
asmregparm long syscall_trace_enter(struct pt_regs *regs)
{
	long ret = 0;

	/*
	 * If we stepped into a sysenter/syscall insn, it trapped in
	 * kernel mode; do_debug() cleared TF and set TIF_SINGLESTEP.
	 * If user-mode had set TF itself, then it's still clear from
	 * do_debug() and we need to set it again to restore the user
	 * state.  If we entered on the slow path, TF was already set.
	 */
	if (test_thread_flag(TIF_SINGLESTEP))
		regs->flags |= X86_EFLAGS_TF;

	/* do the secure computing check first */
	secure_computing(regs->orig_ax);

	if (unlikely(test_thread_flag(TIF_SYSCALL_EMU)))
		ret = -1L;

	if ((ret || test_thread_flag(TIF_SYSCALL_TRACE)) &&
	    tracehook_report_syscall_entry(regs))
		ret = -1L;

	if (unlikely(current->audit_context)) {
		if (IS_IA32)
			audit_syscall_entry(AUDIT_ARCH_I386,
					    regs->orig_ax,
					    regs->bx, regs->cx,
					    regs->dx, regs->si);
#ifdef CONFIG_X86_64
		else
			audit_syscall_entry(AUDIT_ARCH_X86_64,
					    regs->orig_ax,
					    regs->di, regs->si,
					    regs->dx, regs->r10);
#endif
	}

	return ret ?: regs->orig_ax;
}

asmregparm void syscall_trace_leave(struct pt_regs *regs)
{
	if (unlikely(current->audit_context))
		audit_syscall_exit(AUDITSC_RESULT(regs->ax), regs->ax);

	if (test_thread_flag(TIF_SYSCALL_TRACE))
		tracehook_report_syscall_exit(regs, 0);

	/*
	 * If TIF_SYSCALL_EMU is set, we only get here because of
	 * TIF_SINGLESTEP (i.e. this is PTRACE_SYSEMU_SINGLESTEP).
	 * We already reported this syscall instruction in
	 * syscall_trace_enter(), so don't do any more now.
	 */
	if (unlikely(test_thread_flag(TIF_SYSCALL_EMU)))
		return;

	/*
	 * If we are single-stepping, synthesize a trap to follow the
	 * system call instruction.
	 */
	if (test_thread_flag(TIF_SINGLESTEP) &&
	    tracehook_consider_fatal_signal(current, SIGTRAP, SIG_DFL))
		send_sigtrap(current, regs, 0, TRAP_BRKPT);
}

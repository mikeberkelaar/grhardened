/*
 * __get_user functions.
 *
 * (C) Copyright 1998 Linus Torvalds
 * (C) Copyright 2005 Andi Kleen
 * (C) Copyright 2008 Glauber Costa
 *
 * These functions have a non-standard call interface
 * to make them more efficient, especially as they
 * return an error value in addition to the "real"
 * return value.
 */

/*
 * __get_user_X
 *
 * Inputs:	%[r|e]ax contains the address.
 *		The register is modified, but all changes are undone
 *		before returning because the C code doesn't know about it.
 *
 * Outputs:	%[r|e]ax is error code (0 or -EFAULT)
 *		%[r|e]dx contains zero-extended value
 *
 *
 * These functions should not modify any other registers,
 * as they get called from within inline assembly.
 */

#include <linux/linkage.h>
#include <asm/dwarf2.h>
#include <asm/page_types.h>
#include <asm/errno.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/asm.h>
#include <asm/segment.h>
#include <asm/pgtable.h>

	.text
ENTRY(__get_user_1)
	CFI_STARTPROC

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl $(__USER_DS)
	popl %ds
#else
	GET_THREAD_INFO(%_ASM_DX)
	cmp TI_addr_limit(%_ASM_DX),%_ASM_AX
	jae bad_get_user

#if defined(CONFIG_X86_64) && defined(CONFIG_PAX_MEMORY_UDEREF)
	mov $PAX_USER_SHADOW_BASE,%_ASM_DX
	cmp %_ASM_DX,%_ASM_AX
	jae 1234f
	add %_ASM_DX,%_ASM_AX
1234:
#endif

#endif

1:	movzb (%_ASM_AX),%edx

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl %ss
	pop %ds
#endif

	xor %eax,%eax
	ret
	CFI_ENDPROC
ENDPROC(__get_user_1)

ENTRY(__get_user_2)
	CFI_STARTPROC
	add $1,%_ASM_AX

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl $(__USER_DS)
	popl %ds
#else
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_DX)
	cmp TI_addr_limit(%_ASM_DX),%_ASM_AX
	jae bad_get_user

#if defined(CONFIG_X86_64) && defined(CONFIG_PAX_MEMORY_UDEREF)
	mov $PAX_USER_SHADOW_BASE,%_ASM_DX
	cmp %_ASM_DX,%_ASM_AX
	jae 1234f
	add %_ASM_DX,%_ASM_AX
1234:
#endif

#endif

2:	movzwl -1(%_ASM_AX),%edx

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl %ss
	pop %ds
#endif

	xor %eax,%eax
	ret
	CFI_ENDPROC
ENDPROC(__get_user_2)

ENTRY(__get_user_4)
	CFI_STARTPROC
	add $3,%_ASM_AX

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl $(__USER_DS)
	popl %ds
#else
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_DX)
	cmp TI_addr_limit(%_ASM_DX),%_ASM_AX
	jae bad_get_user

#if defined(CONFIG_X86_64) && defined(CONFIG_PAX_MEMORY_UDEREF)
	mov $PAX_USER_SHADOW_BASE,%_ASM_DX
	cmp %_ASM_DX,%_ASM_AX
	jae 1234f
	add %_ASM_DX,%_ASM_AX
1234:
#endif

#endif

3:	mov -3(%_ASM_AX),%edx

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl %ss
	pop %ds
#endif

	xor %eax,%eax
	ret
	CFI_ENDPROC
ENDPROC(__get_user_4)

#ifdef CONFIG_X86_64
ENTRY(__get_user_8)
	CFI_STARTPROC
	add $7,%_ASM_AX
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_DX)
	cmp TI_addr_limit(%_ASM_DX),%_ASM_AX
	jae	bad_get_user

#ifdef CONFIG_PAX_MEMORY_UDEREF
	mov $PAX_USER_SHADOW_BASE,%_ASM_DX
	cmp %_ASM_DX,%_ASM_AX
	jae 1234f
	add %_ASM_DX,%_ASM_AX
1234:
#endif

4:	movq -7(%_ASM_AX),%_ASM_DX
	xor %eax,%eax
	ret
	CFI_ENDPROC
ENDPROC(__get_user_8)
#endif

bad_get_user:
	CFI_STARTPROC

#if defined(CONFIG_X86_32) && defined(CONFIG_PAX_MEMORY_UDEREF)
	pushl %ss
	pop %ds
#endif

	xor %edx,%edx
	mov $(-EFAULT),%_ASM_AX
	ret
	CFI_ENDPROC
END(bad_get_user)

.section __ex_table,"a"
	_ASM_PTR 1b,bad_get_user
	_ASM_PTR 2b,bad_get_user
	_ASM_PTR 3b,bad_get_user
#ifdef CONFIG_X86_64
	_ASM_PTR 4b,bad_get_user
#endif

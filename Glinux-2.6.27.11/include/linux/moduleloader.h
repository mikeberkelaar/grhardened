#ifndef _LINUX_MODULELOADER_H
#define _LINUX_MODULELOADER_H
/* The stuff needed for archs to support modules. */

#include <linux/module.h>
#include <linux/elf.h>

/* These must be implemented by the specific architecture */

/* Adjust arch-specific sections.  Return 0 on success.  */
int module_frob_arch_sections(Elf_Ehdr *hdr,
			      Elf_Shdr *sechdrs,
			      char *secstrings,
			      struct module *mod);

/* Allocator used for allocating struct module, core sections and init
   sections.  Returns NULL on failure. */
void *module_alloc(unsigned long size);

#ifdef CONFIG_PAX_KERNEXEC
void *module_alloc_exec(unsigned long size);
#else
#define module_alloc_exec(x) module_alloc(x)
#endif

/* Free memory returned from module_alloc. */
void module_free(struct module *mod, void *module_region);

#ifdef CONFIG_PAX_KERNEXEC
void module_free_exec(struct module *mod, void *module_region);
#else
#define module_free_exec(x, y) module_free(x, y)
#endif

/* Apply the given relocation to the (simplified) ELF.  Return -error
   or 0. */
int apply_relocate(Elf_Shdr *sechdrs,
		   const char *strtab,
		   unsigned int symindex,
		   unsigned int relsec,
		   struct module *mod);

/* Apply the given add relocation to the (simplified) ELF.  Return
   -error or 0 */
int apply_relocate_add(Elf_Shdr *sechdrs,
		       const char *strtab,
		       unsigned int symindex,
		       unsigned int relsec,
		       struct module *mod);

/* Any final processing of module before access.  Return -error or 0. */
int module_finalize(const Elf_Ehdr *hdr,
		    const Elf_Shdr *sechdrs,
		    struct module *mod);

/* Any cleanup needed when module leaves. */
void module_arch_cleanup(struct module *mod);

#endif

/*
 * Copyright 2002 Andi Kleen, SuSE Labs.
 * Thanks to Ben LaHaise for precious feedback.
 */
#include <linux/highmem.h>
#include <linux/bootmem.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/debugfs.h>

#include <asm/e820.h>
#include <asm/processor.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/uaccess.h>
#include <asm/pgalloc.h>
#include <asm/proto.h>
#include <asm/pat.h>
#include <asm/desc.h>

/*
 * The current flushing context - we pass it instead of 5 arguments:
 */
struct cpa_data {
	unsigned long	*vaddr;
	pgprot_t	mask_set;
	pgprot_t	mask_clr;
	int		numpages;
	int		flags;
	unsigned long	pfn;
	unsigned	force_split : 1;
	int		curpage;
};

/*
 * Serialize cpa() (for !DEBUG_PAGEALLOC which uses large identity mappings)
 * using cpa_lock. So that we don't allow any other cpu, with stale large tlb
 * entries change the page attribute in parallel to some other cpu
 * splitting a large page entry along with changing the attribute.
 */
static DEFINE_SPINLOCK(cpa_lock);

#define CPA_FLUSHTLB 1
#define CPA_ARRAY 2

#ifdef CONFIG_PROC_FS
static unsigned long direct_pages_count[PG_LEVEL_NUM];

void update_page_count(int level, unsigned long pages)
{
	unsigned long flags;

	/* Protect against CPA */
	spin_lock_irqsave(&pgd_lock, flags);
	direct_pages_count[level] += pages;
	spin_unlock_irqrestore(&pgd_lock, flags);
}

static void split_page_count(int level)
{
	direct_pages_count[level]--;
	direct_pages_count[level - 1] += PTRS_PER_PTE;
}

void arch_report_meminfo(struct seq_file *m)
{
	seq_printf(m, "DirectMap4k:    %8lu kB\n",
			direct_pages_count[PG_LEVEL_4K] << 2);
#if defined(CONFIG_X86_64) || defined(CONFIG_X86_PAE)
	seq_printf(m, "DirectMap2M:    %8lu kB\n",
			direct_pages_count[PG_LEVEL_2M] << 11);
#else
	seq_printf(m, "DirectMap4M:    %8lu kB\n",
			direct_pages_count[PG_LEVEL_2M] << 12);
#endif
#ifdef CONFIG_X86_64
	if (direct_gbpages)
		seq_printf(m, "DirectMap1G:    %8lu kB\n",
			direct_pages_count[PG_LEVEL_1G] << 20);
#endif
}
#else
static inline void split_page_count(int level) { }
#endif

#ifdef CONFIG_X86_64

static inline unsigned long highmap_start_pfn(void)
{
	return __pa(_text) >> PAGE_SHIFT;
}

static inline unsigned long highmap_end_pfn(void)
{
	return __pa(roundup((unsigned long)_end, PMD_SIZE)) >> PAGE_SHIFT;
}

#endif

#ifdef CONFIG_DEBUG_PAGEALLOC
# define debug_pagealloc 1
#else
# define debug_pagealloc 0
#endif

static inline int
within(unsigned long addr, unsigned long start, unsigned long end)
{
	return addr >= start && addr < end;
}

/*
 * Flushing functions
 */

/**
 * clflush_cache_range - flush a cache range with clflush
 * @addr:	virtual start address
 * @size:	number of bytes to flush
 *
 * clflush is an unordered instruction which needs fencing with mfence
 * to avoid ordering issues.
 */
void clflush_cache_range(void *vaddr, unsigned int size)
{
	void *vend = vaddr + size - 1;

	mb();

	for (; vaddr < vend; vaddr += boot_cpu_data.x86_clflush_size)
		clflush(vaddr);
	/*
	 * Flush any possible final partial cacheline:
	 */
	clflush(vend);

	mb();
}

static void __cpa_flush_all(void *arg)
{
	unsigned long cache = (unsigned long)arg;

	/*
	 * Flush all to work around Errata in early athlons regarding
	 * large page flushing.
	 */
	__flush_tlb_all();

	if (cache && boot_cpu_data.x86_model >= 4)
		wbinvd();
}

static void cpa_flush_all(unsigned long cache)
{
	BUG_ON(irqs_disabled());

	on_each_cpu(__cpa_flush_all, (void *) cache, 1);
}

static void __cpa_flush_range(void *arg)
{
	/*
	 * We could optimize that further and do individual per page
	 * tlb invalidates for a low number of pages. Caveat: we must
	 * flush the high aliases on 64bit as well.
	 */
	__flush_tlb_all();
}

static void cpa_flush_range(unsigned long start, int numpages, int cache)
{
	unsigned int i, level;
	unsigned long addr;

	BUG_ON(irqs_disabled());
	WARN_ON(PAGE_ALIGN(start) != start);

	on_each_cpu(__cpa_flush_range, NULL, 1);

	if (!cache)
		return;

	/*
	 * We only need to flush on one CPU,
	 * clflush is a MESI-coherent instruction that
	 * will cause all other CPUs to flush the same
	 * cachelines:
	 */
	for (i = 0, addr = start; i < numpages; i++, addr += PAGE_SIZE) {
		pte_t *pte = lookup_address(addr, &level);

		/*
		 * Only flush present addresses:
		 */
		if (pte && (pte_val(*pte) & _PAGE_PRESENT))
			clflush_cache_range((void *) addr, PAGE_SIZE);
	}
}

static void cpa_flush_array(unsigned long *start, int numpages, int cache)
{
	unsigned int i, level;
	unsigned long *addr;

	BUG_ON(irqs_disabled());

	on_each_cpu(__cpa_flush_range, NULL, 1);

	if (!cache)
		return;

	/* 4M threshold */
	if (numpages >= 1024) {
		if (boot_cpu_data.x86_model >= 4)
			wbinvd();
		return;
	}
	/*
	 * We only need to flush on one CPU,
	 * clflush is a MESI-coherent instruction that
	 * will cause all other CPUs to flush the same
	 * cachelines:
	 */
	for (i = 0, addr = start; i < numpages; i++, addr++) {
		pte_t *pte = lookup_address(*addr, &level);

		/*
		 * Only flush present addresses:
		 */
		if (pte && (pte_val(*pte) & _PAGE_PRESENT))
			clflush_cache_range((void *) *addr, PAGE_SIZE);
	}
}

/*
 * Certain areas of memory on x86 require very specific protection flags,
 * for example the BIOS area or kernel text. Callers don't always get this
 * right (again, ioremap() on BIOS memory is not uncommon) so this function
 * checks and fixes these known static required protection bits.
 */
static inline pgprot_t static_protections(pgprot_t prot, unsigned long address,
				   unsigned long pfn)
{
	pgprot_t forbidden = __pgprot(0);

	/*
	 * The BIOS area between 640k and 1Mb needs to be executable for
	 * PCI BIOS based config access (CONFIG_PCI_GOBIOS) support.
	 */
	if (within(pfn, BIOS_BEGIN >> PAGE_SHIFT, BIOS_END >> PAGE_SHIFT))
		pgprot_val(forbidden) |= _PAGE_NX;

	/*
	 * The kernel text needs to be executable for obvious reasons
	 * Does not cover __inittext since that is gone later on. On
	 * 64bit we do not enforce !NX on the low mapping
	 */
	if (within(address, ktla_ktva((unsigned long)_text), ktla_ktva((unsigned long)_etext)))
		pgprot_val(forbidden) |= _PAGE_NX;

	/*
	 * The .rodata section needs to be read-only. Using the pfn
	 * catches all aliases.
	 */
	if (within(pfn, __pa((unsigned long)__start_rodata) >> PAGE_SHIFT,
		   __pa((unsigned long)__end_rodata) >> PAGE_SHIFT))
		pgprot_val(forbidden) |= _PAGE_RW;

	prot = __pgprot(pgprot_val(prot) & ~pgprot_val(forbidden));

	return prot;
}

/*
 * Lookup the page table entry for a virtual address. Return a pointer
 * to the entry and the level of the mapping.
 *
 * Note: We return pud and pmd either when the entry is marked large
 * or when the present bit is not set. Otherwise we would return a
 * pointer to a nonexisting mapping.
 */
pte_t *lookup_address(unsigned long address, unsigned int *level)
{
	pgd_t *pgd = pgd_offset_k(address);
	pud_t *pud;
	pmd_t *pmd;

	*level = PG_LEVEL_NONE;

	if (pgd_none(*pgd))
		return NULL;

	pud = pud_offset(pgd, address);
	if (pud_none(*pud))
		return NULL;

	*level = PG_LEVEL_1G;
	if (pud_large(*pud) || !pud_present(*pud))
		return (pte_t *)pud;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		return NULL;

	*level = PG_LEVEL_2M;
	if (pmd_large(*pmd) || !pmd_present(*pmd))
		return (pte_t *)pmd;

	*level = PG_LEVEL_4K;

	return pte_offset_kernel(pmd, address);
}
EXPORT_SYMBOL_GPL(lookup_address);

/*
 * Set the new pmd in all the pgds we know about:
 */
static void __set_pmd_pte(pte_t *kpte, unsigned long address, pte_t pte)
{

#ifdef CONFIG_PAX_KERNEXEC
	unsigned long cr0;

	pax_open_kernel(cr0);
#endif

	/* change init_mm */
	set_pte_atomic(kpte, pte);

#ifdef CONFIG_PAX_KERNEXEC
	pax_close_kernel(cr0);
#endif

#ifdef CONFIG_X86_32
	if (!SHARED_KERNEL_PMD) {
		struct page *page;

		list_for_each_entry(page, &pgd_list, lru) {
			pgd_t *pgd;
			pud_t *pud;
			pmd_t *pmd;

			pgd = (pgd_t *)page_address(page) + pgd_index(address);
			pud = pud_offset(pgd, address);
			pmd = pmd_offset(pud, address);
			set_pte_atomic((pte_t *)pmd, pte);
		}
	}
#endif
}

static int
try_preserve_large_page(pte_t *kpte, unsigned long address,
			struct cpa_data *cpa)
{
	unsigned long nextpage_addr, numpages, pmask, psize, flags, addr, pfn;
	pte_t new_pte, old_pte, *tmp;
	pgprot_t old_prot, new_prot;
	int i, do_split = 1;
	unsigned int level;

	if (cpa->force_split)
		return 1;

	spin_lock_irqsave(&pgd_lock, flags);
	/*
	 * Check for races, another CPU might have split this page
	 * up already:
	 */
	tmp = lookup_address(address, &level);
	if (tmp != kpte)
		goto out_unlock;

	switch (level) {
	case PG_LEVEL_2M:
		psize = PMD_PAGE_SIZE;
		pmask = PMD_PAGE_MASK;
		break;
#ifdef CONFIG_X86_64
	case PG_LEVEL_1G:
		psize = PUD_PAGE_SIZE;
		pmask = PUD_PAGE_MASK;
		break;
#endif
	default:
		do_split = -EINVAL;
		goto out_unlock;
	}

	/*
	 * Calculate the number of pages, which fit into this large
	 * page starting at address:
	 */
	nextpage_addr = (address + psize) & pmask;
	numpages = (nextpage_addr - address) >> PAGE_SHIFT;
	if (numpages < cpa->numpages)
		cpa->numpages = numpages;

	/*
	 * We are safe now. Check whether the new pgprot is the same:
	 */
	old_pte = *kpte;
	old_prot = new_prot = pte_pgprot(old_pte);

	pgprot_val(new_prot) &= ~pgprot_val(cpa->mask_clr);
	pgprot_val(new_prot) |= pgprot_val(cpa->mask_set);

	/*
	 * old_pte points to the large page base address. So we need
	 * to add the offset of the virtual address:
	 */
	pfn = pte_pfn(old_pte) + ((address & (psize - 1)) >> PAGE_SHIFT);
	cpa->pfn = pfn;

	new_prot = static_protections(new_prot, address, pfn);

	/*
	 * We need to check the full range, whether
	 * static_protection() requires a different pgprot for one of
	 * the pages in the range we try to preserve:
	 */
	addr = address + PAGE_SIZE;
	pfn++;
	for (i = 1; i < cpa->numpages; i++, addr += PAGE_SIZE, pfn++) {
		pgprot_t chk_prot = static_protections(new_prot, addr, pfn);

		if (pgprot_val(chk_prot) != pgprot_val(new_prot))
			goto out_unlock;
	}

	/*
	 * If there are no changes, return. maxpages has been updated
	 * above:
	 */
	if (pgprot_val(new_prot) == pgprot_val(old_prot)) {
		do_split = 0;
		goto out_unlock;
	}

	/*
	 * We need to change the attributes. Check, whether we can
	 * change the large page in one go. We request a split, when
	 * the address is not aligned and the number of pages is
	 * smaller than the number of pages in the large page. Note
	 * that we limited the number of possible pages already to
	 * the number of pages in the large page.
	 */
	if (address == (nextpage_addr - psize) && cpa->numpages == numpages) {
		/*
		 * The address is aligned and the number of pages
		 * covers the full page.
		 */
		new_pte = pfn_pte(pte_pfn(old_pte), canon_pgprot(new_prot));
		__set_pmd_pte(kpte, address, new_pte);
		cpa->flags |= CPA_FLUSHTLB;
		do_split = 0;
	}

out_unlock:
	spin_unlock_irqrestore(&pgd_lock, flags);

	return do_split;
}

static int split_large_page(pte_t *kpte, unsigned long address)
{
	unsigned long flags, pfn, pfninc = 1;
	unsigned int i, level;
	pte_t *pbase, *tmp;
	pgprot_t ref_prot;
	struct page *base;

	if (!debug_pagealloc)
		spin_unlock(&cpa_lock);
	base = alloc_pages(GFP_KERNEL, 0);
	if (!debug_pagealloc)
		spin_lock(&cpa_lock);
	if (!base)
		return -ENOMEM;

	spin_lock_irqsave(&pgd_lock, flags);
	/*
	 * Check for races, another CPU might have split this page
	 * up for us already:
	 */
	tmp = lookup_address(address, &level);
	if (tmp != kpte)
		goto out_unlock;

	pbase = (pte_t *)page_address(base);
	paravirt_alloc_pte(&init_mm, page_to_pfn(base));
	ref_prot = pte_pgprot(pte_clrhuge(*kpte));

#ifdef CONFIG_X86_64
	if (level == PG_LEVEL_1G) {
		pfninc = PMD_PAGE_SIZE >> PAGE_SHIFT;
		pgprot_val(ref_prot) |= _PAGE_PSE;
	}
#endif

	/*
	 * Get the target pfn from the original entry:
	 */
	pfn = pte_pfn(*kpte);
	for (i = 0; i < PTRS_PER_PTE; i++, pfn += pfninc)
		set_pte(&pbase[i], pfn_pte(pfn, ref_prot));

	if (address >= (unsigned long)__va(0) &&
		address < (unsigned long)__va(max_low_pfn_mapped << PAGE_SHIFT))
		split_page_count(level);

#ifdef CONFIG_X86_64
	if (address >= (unsigned long)__va(1UL<<32) &&
		address < (unsigned long)__va(max_pfn_mapped << PAGE_SHIFT))
		split_page_count(level);
#endif

	/*
	 * Install the new, split up pagetable. Important details here:
	 *
	 * On Intel the NX bit of all levels must be cleared to make a
	 * page executable. See section 4.13.2 of Intel 64 and IA-32
	 * Architectures Software Developer's Manual).
	 *
	 * Mark the entry present. The current mapping might be
	 * set to not present, which we preserved above.
	 */
	ref_prot = pte_pgprot(pte_mkexec(pte_clrhuge(*kpte)));
	pgprot_val(ref_prot) |= _PAGE_PRESENT;
	__set_pmd_pte(kpte, address, mk_pte(base, ref_prot));
	base = NULL;

out_unlock:
	/*
	 * If we dropped out via the lookup_address check under
	 * pgd_lock then stick the page back into the pool:
	 */
	if (base)
		__free_page(base);
	spin_unlock_irqrestore(&pgd_lock, flags);

	return 0;
}

static int __change_page_attr(struct cpa_data *cpa, int primary)
{
	unsigned long address;
	int do_split, err;
	unsigned int level;
	pte_t *kpte, old_pte;

	if (cpa->flags & CPA_ARRAY)
		address = cpa->vaddr[cpa->curpage];
	else
		address = *cpa->vaddr;

repeat:
	kpte = lookup_address(address, &level);
	if (!kpte)
		return 0;

	old_pte = *kpte;
	if (!pte_val(old_pte)) {
		if (!primary)
			return 0;
		WARN(1, KERN_WARNING "CPA: called for zero pte. "
		       "vaddr = %lx cpa->vaddr = %lx\n", address,
		       *cpa->vaddr);
		return -EINVAL;
	}

	if (level == PG_LEVEL_4K) {
		pte_t new_pte;
		pgprot_t new_prot = pte_pgprot(old_pte);
		unsigned long pfn = pte_pfn(old_pte);

		pgprot_val(new_prot) &= ~pgprot_val(cpa->mask_clr);
		pgprot_val(new_prot) |= pgprot_val(cpa->mask_set);

		new_prot = static_protections(new_prot, address, pfn);

		/*
		 * We need to keep the pfn from the existing PTE,
		 * after all we're only going to change it's attributes
		 * not the memory it points to
		 */
		new_pte = pfn_pte(pfn, canon_pgprot(new_prot));
		cpa->pfn = pfn;
		/*
		 * Do we really change anything ?
		 */
		if (pte_val(old_pte) != pte_val(new_pte)) {
			set_pte_atomic(kpte, new_pte);
			cpa->flags |= CPA_FLUSHTLB;
		}
		cpa->numpages = 1;
		return 0;
	}

	/*
	 * Check, whether we can keep the large page intact
	 * and just change the pte:
	 */
	do_split = try_preserve_large_page(kpte, address, cpa);
	/*
	 * When the range fits into the existing large page,
	 * return. cp->numpages and cpa->tlbflush have been updated in
	 * try_large_page:
	 */
	if (do_split <= 0)
		return do_split;

	/*
	 * We have to split the large page:
	 */
	err = split_large_page(kpte, address);
	if (!err) {
		/*
	 	 * Do a global flush tlb after splitting the large page
	 	 * and before we do the actual change page attribute in the PTE.
	 	 *
	 	 * With out this, we violate the TLB application note, that says
	 	 * "The TLBs may contain both ordinary and large-page
		 *  translations for a 4-KByte range of linear addresses. This
		 *  may occur if software modifies the paging structures so that
		 *  the page size used for the address range changes. If the two
		 *  translations differ with respect to page frame or attributes
		 *  (e.g., permissions), processor behavior is undefined and may
		 *  be implementation-specific."
	 	 *
	 	 * We do this global tlb flush inside the cpa_lock, so that we
		 * don't allow any other cpu, with stale tlb entries change the
		 * page attribute in parallel, that also falls into the
		 * just split large page entry.
	 	 */
		flush_tlb_all();
		goto repeat;
	}

	return err;
}

static int __change_page_attr_set_clr(struct cpa_data *cpa, int checkalias);

static int cpa_process_alias(struct cpa_data *cpa)
{
	struct cpa_data alias_cpa;
	int ret = 0;
	unsigned long temp_cpa_vaddr, vaddr;

	if (cpa->pfn >= max_pfn_mapped)
		return 0;

#ifdef CONFIG_X86_64
	if (cpa->pfn >= max_low_pfn_mapped && cpa->pfn < (1UL<<(32-PAGE_SHIFT)))
		return 0;
#endif
	/*
	 * No need to redo, when the primary call touched the direct
	 * mapping already:
	 */
	if (cpa->flags & CPA_ARRAY)
		vaddr = cpa->vaddr[cpa->curpage];
	else
		vaddr = *cpa->vaddr;

	if (!(within(vaddr, PAGE_OFFSET,
		    PAGE_OFFSET + (max_low_pfn_mapped << PAGE_SHIFT))
#ifdef CONFIG_X86_64
		|| within(vaddr, PAGE_OFFSET + (1UL<<32),
		    PAGE_OFFSET + (max_pfn_mapped << PAGE_SHIFT))
#endif
	)) {

		alias_cpa = *cpa;
		temp_cpa_vaddr = (unsigned long) __va(cpa->pfn << PAGE_SHIFT);
		alias_cpa.vaddr = &temp_cpa_vaddr;
		alias_cpa.flags &= ~CPA_ARRAY;


		ret = __change_page_attr_set_clr(&alias_cpa, 0);
	}

#ifdef CONFIG_X86_64
	if (ret)
		return ret;
	/*
	 * No need to redo, when the primary call touched the high
	 * mapping already:
	 */
	if (within(vaddr, (unsigned long) _text, (unsigned long) _end))
		return 0;

	/*
	 * If the physical address is inside the kernel map, we need
	 * to touch the high mapped kernel as well:
	 */
	if (!within(cpa->pfn, highmap_start_pfn(), highmap_end_pfn()))
		return 0;

	alias_cpa = *cpa;
	temp_cpa_vaddr = (cpa->pfn << PAGE_SHIFT) + __START_KERNEL_map - phys_base;
	alias_cpa.vaddr = &temp_cpa_vaddr;
	alias_cpa.flags &= ~CPA_ARRAY;

	/*
	 * The high mapping range is imprecise, so ignore the return value.
	 */
	__change_page_attr_set_clr(&alias_cpa, 0);
#endif
	return ret;
}

static int __change_page_attr_set_clr(struct cpa_data *cpa, int checkalias)
{
	int ret, numpages = cpa->numpages;

	while (numpages) {
		/*
		 * Store the remaining nr of pages for the large page
		 * preservation check.
		 */
		cpa->numpages = numpages;
		/* for array changes, we can't use large page */
		if (cpa->flags & CPA_ARRAY)
			cpa->numpages = 1;

		if (!debug_pagealloc)
			spin_lock(&cpa_lock);
		ret = __change_page_attr(cpa, checkalias);
		if (!debug_pagealloc)
			spin_unlock(&cpa_lock);
		if (ret)
			return ret;

		if (checkalias) {
			ret = cpa_process_alias(cpa);
			if (ret)
				return ret;
		}

		/*
		 * Adjust the number of pages with the result of the
		 * CPA operation. Either a large page has been
		 * preserved or a single page update happened.
		 */
		BUG_ON(cpa->numpages > numpages);
		numpages -= cpa->numpages;
		if (cpa->flags & CPA_ARRAY)
			cpa->curpage++;
		else
			*cpa->vaddr += cpa->numpages * PAGE_SIZE;

	}
	return 0;
}

static inline int cache_attr(pgprot_t attr)
{
	return pgprot_val(attr) &
		(_PAGE_PAT | _PAGE_PAT_LARGE | _PAGE_PWT | _PAGE_PCD);
}

static int change_page_attr_set_clr(unsigned long *addr, int numpages,
				    pgprot_t mask_set, pgprot_t mask_clr,
				    int force_split, int array)
{
	struct cpa_data cpa;
	int ret, cache, checkalias;

	/*
	 * Check, if we are requested to change a not supported
	 * feature:
	 */
	mask_set = canon_pgprot(mask_set);
	mask_clr = canon_pgprot(mask_clr);
	if (!pgprot_val(mask_set) && !pgprot_val(mask_clr) && !force_split)
		return 0;

	/* Ensure we are PAGE_SIZE aligned */
	if (!array) {
		if (*addr & ~PAGE_MASK) {
			*addr &= PAGE_MASK;
			/*
			 * People should not be passing in unaligned addresses:
			 */
			WARN_ON_ONCE(1);
		}
	} else {
		int i;
		for (i = 0; i < numpages; i++) {
			if (addr[i] & ~PAGE_MASK) {
				addr[i] &= PAGE_MASK;
				WARN_ON_ONCE(1);
			}
		}
	}

	/* Must avoid aliasing mappings in the highmem code */
	kmap_flush_unused();

	vm_unmap_aliases();

	cpa.vaddr = addr;
	cpa.numpages = numpages;
	cpa.mask_set = mask_set;
	cpa.mask_clr = mask_clr;
	cpa.flags = 0;
	cpa.curpage = 0;
	cpa.force_split = force_split;

	if (array)
		cpa.flags |= CPA_ARRAY;

	/* No alias checking for _NX bit modifications */
	checkalias = (pgprot_val(mask_set) | pgprot_val(mask_clr)) != _PAGE_NX;

	ret = __change_page_attr_set_clr(&cpa, checkalias);

	/*
	 * Check whether we really changed something:
	 */
	if (!(cpa.flags & CPA_FLUSHTLB))
		goto out;

	/*
	 * No need to flush, when we did not set any of the caching
	 * attributes:
	 */
	cache = cache_attr(mask_set);

	/*
	 * On success we use clflush, when the CPU supports it to
	 * avoid the wbindv. If the CPU does not support it and in the
	 * error case we fall back to cpa_flush_all (which uses
	 * wbindv):
	 */
	if (!ret && cpu_has_clflush) {
		if (cpa.flags & CPA_ARRAY)
			cpa_flush_array(addr, numpages, cache);
		else
			cpa_flush_range(*addr, numpages, cache);
	} else
		cpa_flush_all(cache);

out:
	return ret;
}

static inline int change_page_attr_set(unsigned long *addr, int numpages,
				       pgprot_t mask, int array)
{
	return change_page_attr_set_clr(addr, numpages, mask, __pgprot(0), 0,
		array);
}

static inline int change_page_attr_clear(unsigned long *addr, int numpages,
					 pgprot_t mask, int array)
{
	return change_page_attr_set_clr(addr, numpages, __pgprot(0), mask, 0,
		array);
}

int _set_memory_uc(unsigned long addr, int numpages)
{
	/*
	 * for now UC MINUS. see comments in ioremap_nocache()
	 */
	return change_page_attr_set(&addr, numpages,
				    __pgprot(_PAGE_CACHE_UC_MINUS), 0);
}

int set_memory_uc(unsigned long addr, int numpages)
{
	/*
	 * for now UC MINUS. see comments in ioremap_nocache()
	 */
	if (reserve_memtype(__pa(addr), __pa(addr) + numpages * PAGE_SIZE,
			    _PAGE_CACHE_UC_MINUS, NULL))
		return -EINVAL;

	return _set_memory_uc(addr, numpages);
}
EXPORT_SYMBOL(set_memory_uc);

int set_memory_array_uc(unsigned long *addr, int addrinarray)
{
	unsigned long start;
	unsigned long end;
	int i;
	/*
	 * for now UC MINUS. see comments in ioremap_nocache()
	 */
	for (i = 0; i < addrinarray; i++) {
		start = __pa(addr[i]);
		for (end = start + PAGE_SIZE; i < addrinarray - 1; end += PAGE_SIZE) {
			if (end != __pa(addr[i + 1]))
				break;
			i++;
		}
		if (reserve_memtype(start, end, _PAGE_CACHE_UC_MINUS, NULL))
			goto out;
	}

	return change_page_attr_set(addr, addrinarray,
				    __pgprot(_PAGE_CACHE_UC_MINUS), 1);
out:
	for (i = 0; i < addrinarray; i++) {
		unsigned long tmp = __pa(addr[i]);

		if (tmp == start)
			break;
		for (end = tmp + PAGE_SIZE; i < addrinarray - 1; end += PAGE_SIZE) {
			if (end != __pa(addr[i + 1]))
				break;
			i++;
		}
		free_memtype(tmp, end);
	}
	return -EINVAL;
}
EXPORT_SYMBOL(set_memory_array_uc);

int _set_memory_wc(unsigned long addr, int numpages)
{
	return change_page_attr_set(&addr, numpages,
				    __pgprot(_PAGE_CACHE_WC), 0);
}

int set_memory_wc(unsigned long addr, int numpages)
{
	if (!pat_enabled)
		return set_memory_uc(addr, numpages);

	if (reserve_memtype(__pa(addr), __pa(addr) + numpages * PAGE_SIZE,
		_PAGE_CACHE_WC, NULL))
		return -EINVAL;

	return _set_memory_wc(addr, numpages);
}
EXPORT_SYMBOL(set_memory_wc);

int _set_memory_wb(unsigned long addr, int numpages)
{
	return change_page_attr_clear(&addr, numpages,
				      __pgprot(_PAGE_CACHE_MASK), 0);
}

int set_memory_wb(unsigned long addr, int numpages)
{
	free_memtype(__pa(addr), __pa(addr) + numpages * PAGE_SIZE);

	return _set_memory_wb(addr, numpages);
}
EXPORT_SYMBOL(set_memory_wb);

int set_memory_array_wb(unsigned long *addr, int addrinarray)
{
	int i;

	for (i = 0; i < addrinarray; i++) {
		unsigned long start = __pa(addr[i]);
		unsigned long end;

		for (end = start + PAGE_SIZE; i < addrinarray - 1; end += PAGE_SIZE) {
			if (end != __pa(addr[i + 1]))
				break;
			i++;
		}
		free_memtype(start, end);
	}
	return change_page_attr_clear(addr, addrinarray,
				      __pgprot(_PAGE_CACHE_MASK), 1);
}
EXPORT_SYMBOL(set_memory_array_wb);

int set_memory_x(unsigned long addr, int numpages)
{
	return change_page_attr_clear(&addr, numpages, __pgprot(_PAGE_NX), 0);
}
EXPORT_SYMBOL(set_memory_x);

int set_memory_nx(unsigned long addr, int numpages)
{
	return change_page_attr_set(&addr, numpages, __pgprot(_PAGE_NX), 0);
}
EXPORT_SYMBOL(set_memory_nx);

int set_memory_ro(unsigned long addr, int numpages)
{
	return change_page_attr_clear(&addr, numpages, __pgprot(_PAGE_RW), 0);
}
EXPORT_SYMBOL_GPL(set_memory_ro);

int set_memory_rw(unsigned long addr, int numpages)
{
	return change_page_attr_set(&addr, numpages, __pgprot(_PAGE_RW), 0);
}
EXPORT_SYMBOL_GPL(set_memory_rw);

int set_memory_np(unsigned long addr, int numpages)
{
	return change_page_attr_clear(&addr, numpages, __pgprot(_PAGE_PRESENT), 0);
}

int set_memory_4k(unsigned long addr, int numpages)
{
	return change_page_attr_set_clr(&addr, numpages, __pgprot(0),
					__pgprot(0), 1, 0);
}

int set_pages_uc(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_uc(addr, numpages);
}
EXPORT_SYMBOL(set_pages_uc);

int set_pages_wb(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_wb(addr, numpages);
}
EXPORT_SYMBOL(set_pages_wb);

int set_pages_x(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_x(addr, numpages);
}
EXPORT_SYMBOL(set_pages_x);

int set_pages_nx(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_nx(addr, numpages);
}
EXPORT_SYMBOL(set_pages_nx);

int set_pages_ro(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_ro(addr, numpages);
}

int set_pages_rw(struct page *page, int numpages)
{
	unsigned long addr = (unsigned long)page_address(page);

	return set_memory_rw(addr, numpages);
}

#ifdef CONFIG_DEBUG_PAGEALLOC

static int __set_pages_p(struct page *page, int numpages)
{
	unsigned long tempaddr = (unsigned long) page_address(page);
	struct cpa_data cpa = { .vaddr = &tempaddr,
				.numpages = numpages,
				.mask_set = __pgprot(_PAGE_PRESENT | _PAGE_RW),
				.mask_clr = __pgprot(0),
				.flags = 0};

	/*
	 * No alias checking needed for setting present flag. otherwise,
	 * we may need to break large pages for 64-bit kernel text
	 * mappings (this adds to complexity if we want to do this from
	 * atomic context especially). Let's keep it simple!
	 */
	return __change_page_attr_set_clr(&cpa, 0);
}

static int __set_pages_np(struct page *page, int numpages)
{
	unsigned long tempaddr = (unsigned long) page_address(page);
	struct cpa_data cpa = { .vaddr = &tempaddr,
				.numpages = numpages,
				.mask_set = __pgprot(0),
				.mask_clr = __pgprot(_PAGE_PRESENT | _PAGE_RW),
				.flags = 0};

	/*
	 * No alias checking needed for setting not present flag. otherwise,
	 * we may need to break large pages for 64-bit kernel text
	 * mappings (this adds to complexity if we want to do this from
	 * atomic context especially). Let's keep it simple!
	 */
	return __change_page_attr_set_clr(&cpa, 0);
}

void kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (PageHighMem(page))
		return;
	if (!enable) {
		debug_check_no_locks_freed(page_address(page),
					   numpages * PAGE_SIZE);
	}

	/*
	 * If page allocator is not up yet then do not call c_p_a():
	 */
	if (!debug_pagealloc_enabled)
		return;

	/*
	 * The return value is ignored as the calls cannot fail.
	 * Large pages for identity mappings are not used at boot time
	 * and hence no memory allocations during large page split.
	 */
	if (enable)
		__set_pages_p(page, numpages);
	else
		__set_pages_np(page, numpages);

	/*
	 * We should perform an IPI and flush all tlbs,
	 * but that can deadlock->flush only current cpu:
	 */
	__flush_tlb_all();
}

#ifdef CONFIG_HIBERNATION

bool kernel_page_present(struct page *page)
{
	unsigned int level;
	pte_t *pte;

	if (PageHighMem(page))
		return false;

	pte = lookup_address((unsigned long)page_address(page), &level);
	return (pte_val(*pte) & _PAGE_PRESENT);
}

#endif /* CONFIG_HIBERNATION */

#endif /* CONFIG_DEBUG_PAGEALLOC */

/*
 * The testcases use internal knowledge of the implementation that shouldn't
 * be exposed to the rest of the kernel. Include these directly here.
 */
#ifdef CONFIG_CPA_DEBUG
#include "pageattr-test.c"
#endif

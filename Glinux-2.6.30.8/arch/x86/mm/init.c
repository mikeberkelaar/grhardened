#include <linux/ioport.h>
#include <linux/swap.h>

#include <asm/cacheflush.h>
#include <asm/e820.h>
#include <asm/init.h>
#include <asm/page.h>
#include <asm/page_types.h>
#include <asm/sections.h>
#include <asm/setup.h>
#include <asm/system.h>
#include <asm/tlbflush.h>

unsigned long __initdata e820_table_start;
unsigned long __meminitdata e820_table_end;
unsigned long __meminitdata e820_table_top;

int after_bootmem;

int direct_gbpages
#ifdef CONFIG_DIRECT_GBPAGES
				= 1
#endif
;

static void __init find_early_table_space(unsigned long end, int use_pse,
					  int use_gbpages)
{
	unsigned long puds, pmds, ptes, tables, start;

	puds = (end + PUD_SIZE - 1) >> PUD_SHIFT;
	tables = roundup(puds * sizeof(pud_t), PAGE_SIZE);

	if (use_gbpages) {
		unsigned long extra;

		extra = end - ((end>>PUD_SHIFT) << PUD_SHIFT);
		pmds = (extra + PMD_SIZE - 1) >> PMD_SHIFT;
	} else
		pmds = (end + PMD_SIZE - 1) >> PMD_SHIFT;

	tables += roundup(pmds * sizeof(pmd_t), PAGE_SIZE);

	if (use_pse) {
		unsigned long extra;

		extra = end - ((end>>PMD_SHIFT) << PMD_SHIFT);
#ifdef CONFIG_X86_32
		extra += PMD_SIZE;
#endif
		ptes = (extra + PAGE_SIZE - 1) >> PAGE_SHIFT;
	} else
		ptes = (end + PAGE_SIZE - 1) >> PAGE_SHIFT;

	tables += roundup(ptes * sizeof(pte_t), PAGE_SIZE);

#ifdef CONFIG_X86_32
	/* for fixmap */
	tables += roundup(__end_of_fixed_addresses * sizeof(pte_t), PAGE_SIZE);
#endif

	/*
	 * RED-PEN putting page tables only on node 0 could
	 * cause a hotspot and fill up ZONE_DMA. The page tables
	 * need roughly 0.5KB per GB.
	 */
#ifdef CONFIG_X86_32
	start = 0x7000;
	e820_table_start = find_e820_area(start, max_pfn_mapped<<PAGE_SHIFT,
					tables, PAGE_SIZE);
#else /* CONFIG_X86_64 */
	start = 0x8000;
	e820_table_start = find_e820_area(start, end, tables, PAGE_SIZE);
#endif
	if (e820_table_start == -1UL)
		panic("Cannot find space for the kernel page tables");

	e820_table_start >>= PAGE_SHIFT;
	e820_table_end = e820_table_start;
	e820_table_top = e820_table_start + (tables >> PAGE_SHIFT);

	printk(KERN_DEBUG "kernel direct mapping tables up to %lx @ %lx-%lx\n",
		end, e820_table_start << PAGE_SHIFT, e820_table_top << PAGE_SHIFT);
}

struct map_range {
	unsigned long start;
	unsigned long end;
	unsigned page_size_mask;
};

#ifdef CONFIG_X86_32
#define NR_RANGE_MR 3
#else /* CONFIG_X86_64 */
#define NR_RANGE_MR 5
#endif

static int __meminit save_mr(struct map_range *mr, int nr_range,
			     unsigned long start_pfn, unsigned long end_pfn,
			     unsigned long page_size_mask)
{
	if (start_pfn < end_pfn) {
		if (nr_range >= NR_RANGE_MR)
			panic("run out of range for init_memory_mapping\n");
		mr[nr_range].start = start_pfn<<PAGE_SHIFT;
		mr[nr_range].end   = end_pfn<<PAGE_SHIFT;
		mr[nr_range].page_size_mask = page_size_mask;
		nr_range++;
	}

	return nr_range;
}

#ifdef CONFIG_X86_64
static void __init init_gbpages(void)
{
	if (direct_gbpages && cpu_has_gbpages)
		printk(KERN_INFO "Using GB pages for direct mapping\n");
	else
		direct_gbpages = 0;
}
#else
static inline void init_gbpages(void)
{
}
#endif

/*
 * Setup the direct mapping of the physical memory at PAGE_OFFSET.
 * This runs before bootmem is initialized and gets pages directly from
 * the physical memory. To access them they are temporarily mapped.
 */
unsigned long __init_refok init_memory_mapping(unsigned long start,
					       unsigned long end)
{
	unsigned long page_size_mask = 0;
	unsigned long start_pfn, end_pfn;
	unsigned long ret = 0;
	unsigned long pos;

	struct map_range mr[NR_RANGE_MR];
	int nr_range, i;
	int use_pse, use_gbpages;

	printk(KERN_INFO "init_memory_mapping: %016lx-%016lx\n", start, end);

	if (!after_bootmem)
		init_gbpages();

#ifdef CONFIG_DEBUG_PAGEALLOC
	/*
	 * For CONFIG_DEBUG_PAGEALLOC, identity mapping will use small pages.
	 * This will simplify cpa(), which otherwise needs to support splitting
	 * large pages into small in interrupt context, etc.
	 */
	use_pse = use_gbpages = 0;
#else
	use_pse = cpu_has_pse;
	use_gbpages = direct_gbpages;
#endif

#ifdef CONFIG_X86_32
#ifdef CONFIG_X86_PAE
	set_nx();
	if (nx_enabled)
		printk(KERN_INFO "NX (Execute Disable) protection: active\n");
#endif

	/* Enable PSE if available */
	if (cpu_has_pse)
		set_in_cr4(X86_CR4_PSE);

	/* Enable PGE if available */
	if (cpu_has_pge) {
		set_in_cr4(X86_CR4_PGE);
		__supported_pte_mask |= _PAGE_GLOBAL;
	}
#endif

	if (use_gbpages)
		page_size_mask |= 1 << PG_LEVEL_1G;
	if (use_pse)
		page_size_mask |= 1 << PG_LEVEL_2M;

	memset(mr, 0, sizeof(mr));
	nr_range = 0;

	/* head if not big page alignment ? */
	start_pfn = start >> PAGE_SHIFT;
	pos = start_pfn << PAGE_SHIFT;
#ifdef CONFIG_X86_32
	/*
	 * Don't use a large page for the first 2/4MB of memory
	 * because there are often fixed size MTRRs in there
	 * and overlapping MTRRs into large pages can cause
	 * slowdowns.
	 */
	if (pos == 0)
		end_pfn = 1<<(PMD_SHIFT - PAGE_SHIFT);
	else
		end_pfn = ((pos + (PMD_SIZE - 1))>>PMD_SHIFT)
				 << (PMD_SHIFT - PAGE_SHIFT);
#else /* CONFIG_X86_64 */
	end_pfn = ((pos + (PMD_SIZE - 1)) >> PMD_SHIFT)
			<< (PMD_SHIFT - PAGE_SHIFT);
#endif
	if (end_pfn > (end >> PAGE_SHIFT))
		end_pfn = end >> PAGE_SHIFT;
	if (start_pfn < end_pfn) {
		nr_range = save_mr(mr, nr_range, start_pfn, end_pfn, 0);
		pos = end_pfn << PAGE_SHIFT;
	}

	/* big page (2M) range */
	start_pfn = ((pos + (PMD_SIZE - 1))>>PMD_SHIFT)
			 << (PMD_SHIFT - PAGE_SHIFT);
#ifdef CONFIG_X86_32
	end_pfn = (end>>PMD_SHIFT) << (PMD_SHIFT - PAGE_SHIFT);
#else /* CONFIG_X86_64 */
	end_pfn = ((pos + (PUD_SIZE - 1))>>PUD_SHIFT)
			 << (PUD_SHIFT - PAGE_SHIFT);
	if (end_pfn > ((end>>PMD_SHIFT)<<(PMD_SHIFT - PAGE_SHIFT)))
		end_pfn = ((end>>PMD_SHIFT)<<(PMD_SHIFT - PAGE_SHIFT));
#endif

	if (start_pfn < end_pfn) {
		nr_range = save_mr(mr, nr_range, start_pfn, end_pfn,
				page_size_mask & (1<<PG_LEVEL_2M));
		pos = end_pfn << PAGE_SHIFT;
	}

#ifdef CONFIG_X86_64
	/* big page (1G) range */
	start_pfn = ((pos + (PUD_SIZE - 1))>>PUD_SHIFT)
			 << (PUD_SHIFT - PAGE_SHIFT);
	end_pfn = (end >> PUD_SHIFT) << (PUD_SHIFT - PAGE_SHIFT);
	if (start_pfn < end_pfn) {
		nr_range = save_mr(mr, nr_range, start_pfn, end_pfn,
				page_size_mask &
				 ((1<<PG_LEVEL_2M)|(1<<PG_LEVEL_1G)));
		pos = end_pfn << PAGE_SHIFT;
	}

	/* tail is not big page (1G) alignment */
	start_pfn = ((pos + (PMD_SIZE - 1))>>PMD_SHIFT)
			 << (PMD_SHIFT - PAGE_SHIFT);
	end_pfn = (end >> PMD_SHIFT) << (PMD_SHIFT - PAGE_SHIFT);
	if (start_pfn < end_pfn) {
		nr_range = save_mr(mr, nr_range, start_pfn, end_pfn,
				page_size_mask & (1<<PG_LEVEL_2M));
		pos = end_pfn << PAGE_SHIFT;
	}
#endif

	/* tail is not big page (2M) alignment */
	start_pfn = pos>>PAGE_SHIFT;
	end_pfn = end>>PAGE_SHIFT;
	nr_range = save_mr(mr, nr_range, start_pfn, end_pfn, 0);

	/* try to merge same page size and continuous */
	for (i = 0; nr_range > 1 && i < nr_range - 1; i++) {
		unsigned long old_start;
		if (mr[i].end != mr[i+1].start ||
		    mr[i].page_size_mask != mr[i+1].page_size_mask)
			continue;
		/* move it */
		old_start = mr[i].start;
		memmove(&mr[i], &mr[i+1],
			(nr_range - 1 - i) * sizeof(struct map_range));
		mr[i--].start = old_start;
		nr_range--;
	}

	for (i = 0; i < nr_range; i++)
		printk(KERN_DEBUG " %010lx - %010lx page %s\n",
				mr[i].start, mr[i].end,
			(mr[i].page_size_mask & (1<<PG_LEVEL_1G))?"1G":(
			 (mr[i].page_size_mask & (1<<PG_LEVEL_2M))?"2M":"4k"));

	/*
	 * Find space for the kernel direct mapping tables.
	 *
	 * Later we should allocate these tables in the local node of the
	 * memory mapped. Unfortunately this is done currently before the
	 * nodes are discovered.
	 */
	if (!after_bootmem)
		find_early_table_space(end, use_pse, use_gbpages);

#ifdef CONFIG_X86_32
	for (i = 0; i < nr_range; i++)
		kernel_physical_mapping_init(mr[i].start, mr[i].end,
					     mr[i].page_size_mask);
	ret = end;
#else /* CONFIG_X86_64 */
	for (i = 0; i < nr_range; i++)
		ret = kernel_physical_mapping_init(mr[i].start, mr[i].end,
						   mr[i].page_size_mask);
#endif

#ifdef CONFIG_X86_32
	early_ioremap_page_table_range_init();

	load_cr3(swapper_pg_dir);
#endif

#ifdef CONFIG_X86_64
	if (!after_bootmem && !start) {
		pud_t *pud;
		pmd_t *pmd;

		mmu_cr4_features = read_cr4();

		/*
		 * _brk_end cannot change anymore, but it and _end may be
		 * located on different 2M pages. cleanup_highmap(), however,
		 * can only consider _end when it runs, so destroy any
		 * mappings beyond _brk_end here.
		 */
		pud = pud_offset(pgd_offset_k(_brk_end), _brk_end);
		pmd = pmd_offset(pud, _brk_end - 1);
		while (++pmd <= pmd_offset(pud, (unsigned long)_end - 1))
			pmd_clear(pmd);
	}
#endif
	__flush_tlb_all();

	if (!after_bootmem && e820_table_end > e820_table_start)
		reserve_early(e820_table_start << PAGE_SHIFT,
				 e820_table_end << PAGE_SHIFT, "PGTABLE");

	if (!after_bootmem)
		early_memtest(start, end);

	return ret >> PAGE_SHIFT;
}


/*
 * devmem_is_allowed() checks to see if /dev/mem access to a certain address
 * is valid. The argument is a physical page number.
 *
 *
 * On x86, access has to be given to the first megabyte of ram because that area
 * contains bios code and data regions used by X and dosemu and similar apps.
 * Access has to be given to non-kernel-ram areas as well, these contain the PCI
 * mmio resources as well as potential bios/acpi data regions.
 */
int devmem_is_allowed(unsigned long pagenr)
{
	if (!pagenr)
		return 1;
#ifdef CONFIG_VM86
	if (pagenr < (ISA_START_ADDRESS >> PAGE_SHIFT))
		return 1;
#endif
	if ((ISA_START_ADDRESS >> PAGE_SHIFT) <= pagenr && pagenr < (ISA_END_ADDRESS >> PAGE_SHIFT))
		return 1;
	if (iomem_is_exclusive(pagenr << PAGE_SHIFT))
		return 0;
	if (!page_is_ram(pagenr))
		return 1;
	return 0;
}

void free_init_pages(char *what, unsigned long begin, unsigned long end)
{
	unsigned long addr = begin;

	if (addr >= end)
		return;

	/*
	 * If debugging page accesses then do not free this memory but
	 * mark them not present - any buggy init-section access will
	 * create a kernel page fault:
	 */
#ifdef CONFIG_DEBUG_PAGEALLOC
	printk(KERN_INFO "debug: unmapping init memory %08lx..%08lx\n",
		begin, PAGE_ALIGN(end));
	set_memory_np(begin, (end - begin) >> PAGE_SHIFT);
#else
	/*
	 * We just marked the kernel text read only above, now that
	 * we are going to free part of that, we need to make that
	 * writeable first.
	 */
	set_memory_rw(begin, (end - begin) >> PAGE_SHIFT);

	printk(KERN_INFO "Freeing %s: %luk freed\n", what, (end - begin) >> 10);

	for (; addr < end; addr += PAGE_SIZE) {
		ClearPageReserved(virt_to_page(addr));
		init_page_count(virt_to_page(addr));
		memset((void *)(addr & ~(PAGE_SIZE-1)),
			POISON_FREE_INITMEM, PAGE_SIZE);
		free_page(addr);
		totalram_pages++;
	}
#endif
}

void free_initmem(void)
{

#ifdef CONFIG_PAX_KERNEXEC
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

#ifdef CONFIG_X86_32
	/* PaX: limit KERNEL_CS to actual size */
	unsigned long addr, limit;
	struct desc_struct d;
	int cpu;

#ifdef CONFIG_MODULES
	limit = ktva_ktla((unsigned long)&MODULES_END);
#else
	limit = (unsigned long)&_etext;
#endif
	limit = (limit - 1UL) >> PAGE_SHIFT;

	for (cpu = 0; cpu < NR_CPUS; cpu++) {
		pack_descriptor(&d, get_desc_base(&get_cpu_gdt_table(cpu)[GDT_ENTRY_KERNEL_CS]), limit, 0x9B, 0xC);
		write_gdt_entry(get_cpu_gdt_table(cpu), GDT_ENTRY_KERNEL_CS, &d, DESCTYPE_S);
	}

	/* PaX: make KERNEL_CS read-only */
	for (addr = ktla_ktva((unsigned long)&_text); addr < (unsigned long)&_data; addr += PMD_SIZE) {
		pgd = pgd_offset_k(addr);
		pud = pud_offset(pgd, addr);
		pmd = pmd_offset(pud, addr);
		set_pmd(pmd, __pmd(pmd_val(*pmd) & ~_PAGE_RW));
	}
#ifdef CONFIG_X86_PAE
	for (addr = (unsigned long)&__init_begin; addr < (unsigned long)&__init_end; addr += PMD_SIZE) {
		pgd = pgd_offset_k(addr);
		pud = pud_offset(pgd, addr);
		pmd = pmd_offset(pud, addr);
		set_pmd(pmd, __pmd(pmd_val(*pmd) | (_PAGE_NX & __supported_pte_mask)));
	}
#endif
#else
	unsigned long addr, end;

	/* PaX: make kernel code/rodata read-only, rest non-executable */
	for (addr = __START_KERNEL_map; addr < __START_KERNEL_map + KERNEL_IMAGE_SIZE; addr += PMD_SIZE) {
		pgd = pgd_offset_k(addr);
		pud = pud_offset(pgd, addr);
		pmd = pmd_offset(pud, addr);
		if ((unsigned long)_text <= addr && addr < (unsigned long)_data)
			set_pmd(pmd, __pmd(pmd_val(*pmd) & ~_PAGE_RW));
		else
			set_pmd(pmd, __pmd(pmd_val(*pmd) | (_PAGE_NX & __supported_pte_mask)));
	}

	addr = (unsigned long)__va(__pa(__START_KERNEL_map));
	end = addr + KERNEL_IMAGE_SIZE;
	for (; addr < end; addr += PMD_SIZE) {
		pgd = pgd_offset_k(addr);
		pud = pud_offset(pgd, addr);
		pmd = pmd_offset(pud, addr);
		if ((unsigned long)__va(__pa(_text)) <= addr && addr < (unsigned long)__va(__pa(_data)))
			set_pmd(pmd, __pmd(pmd_val(*pmd) & ~_PAGE_RW));
		else
			set_pmd(pmd, __pmd(pmd_val(*pmd) | (_PAGE_NX & __supported_pte_mask)));
	}
#endif

	flush_tlb_all();
#endif

	free_init_pages("unused kernel memory",
			(unsigned long)(&__init_begin),
			(unsigned long)(&__init_end));
}

#ifdef CONFIG_BLK_DEV_INITRD
void free_initrd_mem(unsigned long start, unsigned long end)
{
	free_init_pages("initrd memory", start, end);
}
#endif

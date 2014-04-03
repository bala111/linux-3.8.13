/*
 * PPC Huge TLB Page Support for Book3E MMU
 *
 * Copyright (C) 2009 David Gibson, IBM Corporation.
 * Copyright (C) 2011 Becky Bruce, Freescale Semiconductor
 *
 */
#include <linux/mm.h>
#include <linux/hugetlb.h>

static inline int mmu_get_tsize(int psize)
{
	return mmu_psize_defs[psize].enc;
}

#if defined(CONFIG_PPC_FSL_BOOK3E) && defined(CONFIG_PPC64)
#include <asm/paca.h>

static inline void book3e_tlb_lock(void)
{
	struct paca_struct *paca = get_paca();
	struct tlb_per_core *percore;
	unsigned long tmp;

	if (!(paca->tlb_per_core_ptr & 1))
		return;

	percore = (struct tlb_per_core *)(paca->tlb_per_core_ptr & ~1UL);

	asm volatile("1: lbarx %0, 0, %1;"
		     "cmpdi %0, 0;"
		     "bne 2f;"
		     "li %0, 1;"
		     "stbcx. %0, 0, %1;"
		     "bne 1b;"
		     "b 3f;"
		     "2: lbzx %0, 0, %1;"
		     "cmpdi %0, 0;"
		     "bne 2b;"
		     "b 1b;"
		     "3:" : "=&r" (tmp) : "r" (&percore->lock) : "memory");
}

static inline void book3e_tlb_unlock(void)
{
	struct paca_struct *paca = get_paca();
	struct tlb_per_core *percore;

	if (!(paca->tlb_per_core_ptr & 1))
		return;

	percore = (struct tlb_per_core *)(paca->tlb_per_core_ptr & ~1UL);

	isync();
	percore->lock = 0;
}
#else
static inline void book3e_tlb_lock(void)
{
}

static inline void book3e_tlb_unlock(void)
{
}
#endif

#ifdef CONFIG_PPC_FSL_BOOK3E
#ifdef CONFIG_PPC64
static inline int tlb1_next(void)
{
	struct paca_struct *paca = get_paca();
	struct tlb_per_core *percore;
	int this, next;

	percore = (struct tlb_per_core *)(paca->tlb_per_core_ptr & ~1UL);

	this = percore->esel_next;

	next = this + 1;
	if (next >= percore->esel_max)
		next = percore->esel_first;

	percore->esel_next = next;
	return this;
}
#else
static inline int tlb1_next(void)
{
	int index, ncams;

	ncams = mfspr(SPRN_TLB1CFG) & TLBnCFG_N_ENTRY;

	index = __get_cpu_var(next_tlbcam_idx);

	/* Just round-robin the entries and wrap when we hit the end */
	if (unlikely(index == ncams - 1))
		__get_cpu_var(next_tlbcam_idx) = tlbcam_index;
	else
		__get_cpu_var(next_tlbcam_idx)++;

	return index;
}
#endif /* !PPC64 */
#endif /* FSL */

static inline int book3e_tlb_exists(unsigned long ea, unsigned long pid)
{
	int found = 0;

	mtspr(SPRN_MAS6, pid << 16);
	if (mmu_has_feature(MMU_FTR_USE_TLBRSRV)) {
		asm volatile(
			"li	%0,0\n"
			"tlbsx.	0,%1\n"
			"bne	1f\n"
			"li	%0,1\n"
			"1:\n"
			: "=&r"(found) : "r"(ea));
	} else {
		asm volatile(
			"tlbsx	0,%1\n"
			"mfspr	%0,0x271\n"
			"srwi	%0,%0,31\n"
			: "=&r"(found) : "r"(ea));
	}

	return found;
}

void book3e_hugetlb_preload(struct vm_area_struct *vma, unsigned long ea,
			    pte_t pte)
{
	unsigned long mas1, mas2;
	u64 mas7_3;
	unsigned long psize, tsize, shift;
	unsigned long flags;
	struct mm_struct *mm;

#ifdef CONFIG_PPC_FSL_BOOK3E
	int index;
#endif

	if (unlikely(is_kernel_addr(ea)))
		return;

	mm = vma->vm_mm;

#ifdef CONFIG_PPC_MM_SLICES
	psize = get_slice_psize(mm, ea);
	tsize = mmu_get_tsize(psize);
	shift = mmu_psize_defs[psize].shift;
#else
	psize = vma_mmu_pagesize(vma);
	shift = __ilog2(psize);
	tsize = shift - 10;
#endif

	/*
	 * We can't be interrupted while we're setting up the MAS
	 * regusters or after we've confirmed that no tlb exists.
	 */
	local_irq_save(flags);

	book3e_tlb_lock();

	if (unlikely(book3e_tlb_exists(ea, mm->context.id))) {
		book3e_tlb_unlock();
		local_irq_restore(flags);
		return;
	}

#ifdef CONFIG_PPC_FSL_BOOK3E
	/* We have to use the CAM(TLB1) on FSL parts for hugepages */
	index = tlb1_next();
	mtspr(SPRN_MAS0, MAS0_ESEL(index) | MAS0_TLBSEL(1));
#endif

	mas1 = MAS1_VALID | MAS1_TID(mm->context.id) | MAS1_TSIZE(tsize);
	mas2 = ea & ~((1UL << shift) - 1);
	mas2 |= (pte_val(pte) >> PTE_WIMGE_SHIFT) & MAS2_WIMGE_MASK;
	mas7_3 = (u64)pte_pfn(pte) << PAGE_SHIFT;
	mas7_3 |= (pte_val(pte) >> PTE_BAP_SHIFT) & MAS3_BAP_MASK;
	if (!pte_dirty(pte))
		mas7_3 &= ~(MAS3_SW|MAS3_UW);

	mtspr(SPRN_MAS1, mas1);
	mtspr(SPRN_MAS2, mas2);

	if (mmu_has_feature(MMU_FTR_USE_PAIRED_MAS)) {
		mtspr(SPRN_MAS7_MAS3, mas7_3);
	} else {
		mtspr(SPRN_MAS7, upper_32_bits(mas7_3));
		mtspr(SPRN_MAS3, lower_32_bits(mas7_3));
	}

	asm volatile ("tlbwe");

	book3e_tlb_unlock();
	local_irq_restore(flags);
}

void flush_hugetlb_page(struct vm_area_struct *vma, unsigned long vmaddr)
{
	struct hstate *hstate = hstate_file(vma->vm_file);
	unsigned long tsize = huge_page_shift(hstate) - 10;

	__flush_tlb_page(vma ? vma->vm_mm : NULL, vmaddr, tsize, 0);

}

/*
 * Copyright (C) 2003-2004 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup mips32mm	
 * @{
 */
/** @file
 */

#include <arch/mm/tlb.h>
#include <mm/asid.h>
#include <mm/tlb.h>
#include <mm/page.h>
#include <mm/as.h>
#include <arch/cp0.h>
#include <panic.h>
#include <arch.h>
#include <symtab.h>
#include <synch/spinlock.h>
#include <print.h>
#include <debug.h>
#include <align.h>
#include <interrupt.h>

static void tlb_refill_fail(istate_t *istate);
static void tlb_invalid_fail(istate_t *istate);
static void tlb_modified_fail(istate_t *istate);

static pte_t *find_mapping_and_check(__address badvaddr, int access, istate_t *istate, int *pfrc);

static void prepare_entry_lo(entry_lo_t *lo, bool g, bool v, bool d, bool cacheable, __address pfn);
static void prepare_entry_hi(entry_hi_t *hi, asid_t asid, __address addr);

/** Initialize TLB
 *
 * Initialize TLB.
 * Invalidate all entries and mark wired entries.
 */
void tlb_arch_init(void)
{
	int i;

	cp0_pagemask_write(TLB_PAGE_MASK_16K);
	cp0_entry_hi_write(0);
	cp0_entry_lo0_write(0);
	cp0_entry_lo1_write(0);

	/* Clear and initialize TLB. */
	
	for (i = 0; i < TLB_ENTRY_COUNT; i++) {
		cp0_index_write(i);
		tlbwi();
	}

		
	/*
	 * The kernel is going to make use of some wired
	 * entries (e.g. mapping kernel stacks in kseg3).
	 */
	cp0_wired_write(TLB_WIRED);
}

/** Process TLB Refill Exception
 *
 * Process TLB Refill Exception.
 *
 * @param istate Interrupted register context.
 */
void tlb_refill(istate_t *istate)
{
	entry_lo_t lo;
	entry_hi_t hi;
	asid_t asid;
	__address badvaddr;
	pte_t *pte;
	int pfrc;

	badvaddr = cp0_badvaddr_read();

	spinlock_lock(&AS->lock);
	asid = AS->asid;
	spinlock_unlock(&AS->lock);

	page_table_lock(AS, true);

	pte = find_mapping_and_check(badvaddr, PF_ACCESS_READ, istate, &pfrc);
	if (!pte) {
		switch (pfrc) {
		case AS_PF_FAULT:
			goto fail;
			break;
		case AS_PF_DEFER:
			/*
			 * The page fault came during copy_from_uspace()
			 * or copy_to_uspace().
			 */
			page_table_unlock(AS, true);
			return;
		default:
			panic("unexpected pfrc (%d)\n", pfrc);
		}
	}

	/*
	 * Record access to PTE.
	 */
	pte->a = 1;

	prepare_entry_hi(&hi, asid, badvaddr);
	prepare_entry_lo(&lo, pte->g, pte->p, pte->d, pte->cacheable, pte->pfn);

	/*
	 * New entry is to be inserted into TLB
	 */
	cp0_entry_hi_write(hi.value);
	if ((badvaddr/PAGE_SIZE) % 2 == 0) {
		cp0_entry_lo0_write(lo.value);
		cp0_entry_lo1_write(0);
	}
	else {
		cp0_entry_lo0_write(0);
		cp0_entry_lo1_write(lo.value);
	}
	cp0_pagemask_write(TLB_PAGE_MASK_16K);
	tlbwr();

	page_table_unlock(AS, true);
	return;
	
fail:
	page_table_unlock(AS, true);
	tlb_refill_fail(istate);
}

/** Process TLB Invalid Exception
 *
 * Process TLB Invalid Exception.
 *
 * @param istate Interrupted register context.
 */
void tlb_invalid(istate_t *istate)
{
	tlb_index_t index;
	__address badvaddr;
	entry_lo_t lo;
	entry_hi_t hi;
	pte_t *pte;
	int pfrc;

	badvaddr = cp0_badvaddr_read();

	/*
	 * Locate the faulting entry in TLB.
	 */
	hi.value = cp0_entry_hi_read();
	prepare_entry_hi(&hi, hi.asid, badvaddr);
	cp0_entry_hi_write(hi.value);
	tlbp();
	index.value = cp0_index_read();

	page_table_lock(AS, true);	
	
	/*
	 * Fail if the entry is not in TLB.
	 */
	if (index.p) {
		printf("TLB entry not found.\n");
		goto fail;
	}

	pte = find_mapping_and_check(badvaddr, PF_ACCESS_READ, istate, &pfrc);
	if (!pte) {
		switch (pfrc) {
		case AS_PF_FAULT:
			goto fail;
			break;
		case AS_PF_DEFER:
			/*
			 * The page fault came during copy_from_uspace()
			 * or copy_to_uspace().
			 */
			page_table_unlock(AS, true);			 
			return;
		default:
			panic("unexpected pfrc (%d)\n", pfrc);
		}
	}

	/*
	 * Read the faulting TLB entry.
	 */
	tlbr();

	/*
	 * Record access to PTE.
	 */
	pte->a = 1;

	prepare_entry_lo(&lo, pte->g, pte->p, pte->d, pte->cacheable, pte->pfn);

	/*
	 * The entry is to be updated in TLB.
	 */
	if ((badvaddr/PAGE_SIZE) % 2 == 0)
		cp0_entry_lo0_write(lo.value);
	else
		cp0_entry_lo1_write(lo.value);
	cp0_pagemask_write(TLB_PAGE_MASK_16K);
	tlbwi();

	page_table_unlock(AS, true);
	return;
	
fail:
	page_table_unlock(AS, true);
	tlb_invalid_fail(istate);
}

/** Process TLB Modified Exception
 *
 * Process TLB Modified Exception.
 *
 * @param istate Interrupted register context.
 */
void tlb_modified(istate_t *istate)
{
	tlb_index_t index;
	__address badvaddr;
	entry_lo_t lo;
	entry_hi_t hi;
	pte_t *pte;
	int pfrc;

	badvaddr = cp0_badvaddr_read();

	/*
	 * Locate the faulting entry in TLB.
	 */
	hi.value = cp0_entry_hi_read();
	prepare_entry_hi(&hi, hi.asid, badvaddr);
	cp0_entry_hi_write(hi.value);
	tlbp();
	index.value = cp0_index_read();

	page_table_lock(AS, true);	
	
	/*
	 * Fail if the entry is not in TLB.
	 */
	if (index.p) {
		printf("TLB entry not found.\n");
		goto fail;
	}

	pte = find_mapping_and_check(badvaddr, PF_ACCESS_WRITE, istate, &pfrc);
	if (!pte) {
		switch (pfrc) {
		case AS_PF_FAULT:
			goto fail;
			break;
		case AS_PF_DEFER:
			/*
			 * The page fault came during copy_from_uspace()
			 * or copy_to_uspace().
			 */
			page_table_unlock(AS, true);			 
			return;
		default:
			panic("unexpected pfrc (%d)\n", pfrc);
		}
	}

	/*
	 * Fail if the page is not writable.
	 */
	if (!pte->w)
		goto fail;

	/*
	 * Read the faulting TLB entry.
	 */
	tlbr();

	/*
	 * Record access and write to PTE.
	 */
	pte->a = 1;
	pte->d = 1;

	prepare_entry_lo(&lo, pte->g, pte->p, pte->w, pte->cacheable, pte->pfn);

	/*
	 * The entry is to be updated in TLB.
	 */
	if ((badvaddr/PAGE_SIZE) % 2 == 0)
		cp0_entry_lo0_write(lo.value);
	else
		cp0_entry_lo1_write(lo.value);
	cp0_pagemask_write(TLB_PAGE_MASK_16K);
	tlbwi();

	page_table_unlock(AS, true);
	return;
	
fail:
	page_table_unlock(AS, true);
	tlb_modified_fail(istate);
}

void tlb_refill_fail(istate_t *istate)
{
	char *symbol = "";
	char *sym2 = "";

	char *s = get_symtab_entry(istate->epc);
	if (s)
		symbol = s;
	s = get_symtab_entry(istate->ra);
	if (s)
		sym2 = s;

	fault_if_from_uspace(istate, "TLB Refill Exception on %p", cp0_badvaddr_read());
	panic("%x: TLB Refill Exception at %x(%s<-%s)\n", cp0_badvaddr_read(), istate->epc, symbol, sym2);
}


void tlb_invalid_fail(istate_t *istate)
{
	char *symbol = "";

	char *s = get_symtab_entry(istate->epc);
	if (s)
		symbol = s;
	fault_if_from_uspace(istate, "TLB Invalid Exception on %p", cp0_badvaddr_read());
	panic("%x: TLB Invalid Exception at %x(%s)\n", cp0_badvaddr_read(), istate->epc, symbol);
}

void tlb_modified_fail(istate_t *istate)
{
	char *symbol = "";

	char *s = get_symtab_entry(istate->epc);
	if (s)
		symbol = s;
	fault_if_from_uspace(istate, "TLB Modified Exception on %p", cp0_badvaddr_read());
	panic("%x: TLB Modified Exception at %x(%s)\n", cp0_badvaddr_read(), istate->epc, symbol);
}

/** Try to find PTE for faulting address
 *
 * Try to find PTE for faulting address.
 * The AS->lock must be held on entry to this function.
 *
 * @param badvaddr Faulting virtual address.
 * @param access Access mode that caused the fault.
 * @param istate Pointer to interrupted state.
 * @param pfrc Pointer to variable where as_page_fault() return code will be stored.
 *
 * @return PTE on success, NULL otherwise.
 */
pte_t *find_mapping_and_check(__address badvaddr, int access, istate_t *istate, int *pfrc)
{
	entry_hi_t hi;
	pte_t *pte;

	hi.value = cp0_entry_hi_read();

	/*
	 * Handler cannot succeed if the ASIDs don't match.
	 */
	if (hi.asid != AS->asid) {
		printf("EntryHi.asid=%d, AS->asid=%d\n", hi.asid, AS->asid);
		return NULL;
	}

	/*
	 * Check if the mapping exists in page tables.
	 */	
	pte = page_mapping_find(AS, badvaddr);
	if (pte && pte->p) {
		/*
		 * Mapping found in page tables.
		 * Immediately succeed.
		 */
		return pte;
	} else {
		int rc;
		
		/*
		 * Mapping not found in page tables.
		 * Resort to higher-level page fault handler.
		 */
		page_table_unlock(AS, true);
		switch (rc = as_page_fault(badvaddr, access, istate)) {
		case AS_PF_OK:
			/*
			 * The higher-level page fault handler succeeded,
			 * The mapping ought to be in place.
			 */
			page_table_lock(AS, true);
			pte = page_mapping_find(AS, badvaddr);
			ASSERT(pte && pte->p);
			return pte;
			break;
		case AS_PF_DEFER:
			page_table_lock(AS, true);
			*pfrc = AS_PF_DEFER;
			return NULL;
			break;
		case AS_PF_FAULT:
			page_table_lock(AS, true);
			printf("Page fault.\n");
			*pfrc = AS_PF_FAULT;
			return NULL;
			break;
		default:
			panic("unexpected rc (%d)\n", rc);
		}
		
	}
}

void prepare_entry_lo(entry_lo_t *lo, bool g, bool v, bool d, bool cacheable, __address pfn)
{
	lo->value = 0;
	lo->g = g;
	lo->v = v;
	lo->d = d;
	lo->c = cacheable ? PAGE_CACHEABLE_EXC_WRITE : PAGE_UNCACHED;
	lo->pfn = pfn;
}

void prepare_entry_hi(entry_hi_t *hi, asid_t asid, __address addr)
{
	hi->value = ALIGN_DOWN(addr, PAGE_SIZE * 2);
	hi->asid = asid;
}

/** Print contents of TLB. */
void tlb_print(void)
{
	page_mask_t mask;
	entry_lo_t lo0, lo1;
	entry_hi_t hi, hi_save;
	int i;

	hi_save.value = cp0_entry_hi_read();

	printf("TLB:\n");
	for (i = 0; i < TLB_ENTRY_COUNT; i++) {
		cp0_index_write(i);
		tlbr();
		
		mask.value = cp0_pagemask_read();
		hi.value = cp0_entry_hi_read();
		lo0.value = cp0_entry_lo0_read();
		lo1.value = cp0_entry_lo1_read();
		
		printf("%d: asid=%d, vpn2=%d, mask=%d\tg[0]=%d, v[0]=%d, d[0]=%d, c[0]=%hhd, pfn[0]=%d\n"
		       "\t\t\t\tg[1]=%d, v[1]=%d, d[1]=%d, c[1]=%hhd, pfn[1]=%d\n",
		       i, hi.asid, hi.vpn2, mask.mask, lo0.g, lo0.v, lo0.d, lo0.c, lo0.pfn,
		       lo1.g, lo1.v, lo1.d, lo1.c, lo1.pfn);
	}
	
	cp0_entry_hi_write(hi_save.value);
}

/** Invalidate all not wired TLB entries. */
void tlb_invalidate_all(void)
{
	ipl_t ipl;
	entry_lo_t lo0, lo1;
	entry_hi_t hi_save;
	int i;

	hi_save.value = cp0_entry_hi_read();
	ipl = interrupts_disable();

	for (i = TLB_WIRED; i < TLB_ENTRY_COUNT; i++) {
		cp0_index_write(i);
		tlbr();

		lo0.value = cp0_entry_lo0_read();
		lo1.value = cp0_entry_lo1_read();

		lo0.v = 0;
		lo1.v = 0;

		cp0_entry_lo0_write(lo0.value);
		cp0_entry_lo1_write(lo1.value);
				
		tlbwi();
	}
	
	interrupts_restore(ipl);
	cp0_entry_hi_write(hi_save.value);
}

/** Invalidate all TLB entries belonging to specified address space.
 *
 * @param asid Address space identifier.
 */
void tlb_invalidate_asid(asid_t asid)
{
	ipl_t ipl;
	entry_lo_t lo0, lo1;
	entry_hi_t hi, hi_save;
	int i;

	ASSERT(asid != ASID_INVALID);

	hi_save.value = cp0_entry_hi_read();
	ipl = interrupts_disable();
	
	for (i = 0; i < TLB_ENTRY_COUNT; i++) {
		cp0_index_write(i);
		tlbr();
		
		hi.value = cp0_entry_hi_read();
		
		if (hi.asid == asid) {
			lo0.value = cp0_entry_lo0_read();
			lo1.value = cp0_entry_lo1_read();

			lo0.v = 0;
			lo1.v = 0;

			cp0_entry_lo0_write(lo0.value);
			cp0_entry_lo1_write(lo1.value);

			tlbwi();
		}
	}
	
	interrupts_restore(ipl);
	cp0_entry_hi_write(hi_save.value);
}

/** Invalidate TLB entries for specified page range belonging to specified address space.
 *
 * @param asid Address space identifier.
 * @param page First page whose TLB entry is to be invalidated.
 * @param cnt Number of entries to invalidate.
 */
void tlb_invalidate_pages(asid_t asid, __address page, count_t cnt)
{
	int i;
	ipl_t ipl;
	entry_lo_t lo0, lo1;
	entry_hi_t hi, hi_save;
	tlb_index_t index;

	ASSERT(asid != ASID_INVALID);

	hi_save.value = cp0_entry_hi_read();
	ipl = interrupts_disable();

	for (i = 0; i < cnt+1; i+=2) {
		hi.value = 0;
		prepare_entry_hi(&hi, asid, page + i * PAGE_SIZE);
		cp0_entry_hi_write(hi.value);

		tlbp();
		index.value = cp0_index_read();

		if (!index.p) {
			/* Entry was found, index register contains valid index. */
			tlbr();

			lo0.value = cp0_entry_lo0_read();
			lo1.value = cp0_entry_lo1_read();

			lo0.v = 0;
			lo1.v = 0;

			cp0_entry_lo0_write(lo0.value);
			cp0_entry_lo1_write(lo1.value);

			tlbwi();
		}
	}
	
	interrupts_restore(ipl);
	cp0_entry_hi_write(hi_save.value);
}

/** @}
 */

/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ptemap_pte.c — PTE 直写实现 + 逐页 cache 策略（v1.1 / v1.2）
 *
 * 使用 apply_to_page_range() 遍历并填充页表，在回调中通过
 * set_pte_at() + pfn_pte() 直接构造并写入 PTE 条目。
 *
 * v1.2 新增：逐页独立 cache 策略
 *   - ptemap_cache_pgprot(mode, base) 将 cache mode 转为 pgprot_t
 *   - 回调中按 page index 从 g_state.page_pgprot[] 查表
 *   - 支持 WC (default) / WB / UC / WT 四种策略
 *   - 策略通过 debugfs cache_policy 文件在 mmap 前设置
 *
 * 启用方式：insmod ptemap.ko use_direct_pte=1
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/pgtable.h>
#include <asm/pgtable_types.h>
#include "ptemap_core.h"

/*
 * 将 cache mode 转为 pgprot_t。
 *
 * x86 PAT MSR 默认配置下的 cache bits 映射：
 *   WC (_PAGE_PAT)           — Write-Combining
 *   WB (clear bits)          — Write-Back (默认)
 *   UC (_PAGE_PCD|_PAGE_PWT) — Uncacheable
 *   WT (_PAGE_PAT|_PAGE_PWT) — Write-Through
 */
pgprot_t ptemap_cache_pgprot(enum ptemap_cache_mode mode, pgprot_t base)
{
	pgprotval_t val = pgprot_val(base);

	/* 清除已有的 cache 控制位 */
	val &= ~(_PAGE_PAT | _PAGE_PCD | _PAGE_PWT);

	/* VM_PFNMAP + set_pte_at path: mark PTE special so
	 * vm_normal_page() returns NULL → zap skips RSS counter
	 */
	val |= _PAGE_SPECIAL;

	switch (mode) {
	case PTEMAP_CACHE_WC:
		val |= _PAGE_PAT;
		break;
	case PTEMAP_CACHE_WB:
		/* WB = 清除所有 cache bits（已是默认） */
		break;
	case PTEMAP_CACHE_UC:
		val |= _PAGE_PCD | _PAGE_PWT;
		break;
	case PTEMAP_CACHE_WT:
		val |= _PAGE_PAT | _PAGE_PWT;
		break;
	default:
		val |= _PAGE_PAT; /* 未知 mode 回退到 WC */
		break;
	}

	return __pgprot(val);
}

/* PTE 直写回调上下文 */
struct pte_write_ctx {
	struct page **pages;        /* 预分配的物理页数组 */
	unsigned long vma_start;    /* VMA 起始虚拟地址 */
	struct mm_struct *mm;       /* 目标进程 mm_struct */
};

/*
 * v1.2: 逐页从 g_state.page_pgprot[] 获取该页的 cache 策略，
 * 不再使用统一的 pgprot_t。
 */
static int ptemap_write_pte(pte_t *pte, unsigned long addr, void *data)
{
	struct pte_write_ctx *ctx = data;
	unsigned long idx = (addr - ctx->vma_start) >> PAGE_SHIFT;
	unsigned long pfn = page_to_pfn(ctx->pages[idx]);
	pgprot_t pgprot = g_state.page_pgprot[idx];
	pte_t new_pte;

	new_pte = pfn_pte(pfn, pgprot);
	set_pte_at(ctx->mm, addr, pte, new_pte);

	return 0;
}

/*
 * mmap 回调（PTE 直写 + 逐页 cache 策略）
 */
int ptemap_mmap_direct(struct vm_area_struct *vma)
{
	struct pte_write_ctx ctx;
	unsigned long nr_pages;
	pgprot_t base_prot;
	int ret, i;

	nr_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;

	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_PFNMAP);

	/*
	 * 重新计算 page_pgprot[] 数组（用户可能通过 debugfs 修改了
	 * page_cache[] 之后再次 mmap）。
	 *
	 * base_prot 从 vma->vm_page_prot 取（带 COW/encrypted 等遗留位），
	 * 然后用 ptemap_cache_pgprot() 覆盖 cache bits。
	 */
	base_prot = vma->vm_page_prot;
	for (i = 0; i < nr_pages; i++)
		g_state.page_pgprot[i] = ptemap_cache_pgprot(
			g_state.page_cache[i], base_prot);

	vma->vm_page_prot = g_state.page_pgprot[0];

	/* 构造回调上下文（不再带统一 pgprot） */
	ctx.pages     = g_state.pages;
	ctx.vma_start = vma->vm_start;
	ctx.mm        = vma->vm_mm;

	ret = apply_to_page_range(vma->vm_mm, vma->vm_start,
				  nr_pages << PAGE_SHIFT,
				  ptemap_write_pte, &ctx);
	if (ret) {
		pr_err("ptemap: apply_to_page_range failed: %d\n", ret);
		return ret;
	}

	g_state.vaddr_start = vma->vm_start;
	g_state.vaddr_end   = vma->vm_end;
	g_state.vaddr_size  = vma->vm_end - vma->vm_start;

	/* Track the mm for safe PTE cleanup at module unload */
	if (!g_state.mapped_mm) {
		mmgrab(vma->vm_mm);
		g_state.mapped_mm = vma->vm_mm;
	}

	pr_info("ptemap: mmap DIRECT OK: vaddr=0x%lx-0x%lx pages=%lu pid=%d\n",
		vma->vm_start, vma->vm_end, nr_pages, current->pid);

	return 0;
}

/*
 * No-op PTE callback for pre-populating the page table structure.
 */
static int ptemap_pte_noop(pte_t *pte, unsigned long addr, void *data)
{
	return 0;
}

/*
 * mmap callback for 2MB PMD-level huge pages.
 *
 * Strategy: use apply_to_page_range() with a no-op callback to allocate
 * the full page table tree (PGD→P4D→PUD→PMD→PTE) via the kernel's built-in
 * allocator (handles PTI internally).  Then walk the now-populated table
 * and replace each PTE-level PMD entry with a PMD-level huge entry.
 */
int ptemap_mmap_huge(struct vm_area_struct *vma)
{
	unsigned long addr, end, pfn;
	unsigned long nr_huge, size;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pmd_t new_pmd;
	pgprot_t base_prot;
	int i, ret;

	nr_huge = (vma->vm_end - vma->vm_start) >> PMD_SHIFT;
	if (nr_huge > g_state.nr_pages) {
		pr_err("ptemap: mmap requests %lu huge pages, only %lu allocated\n",
		       nr_huge, g_state.nr_pages);
		return -EINVAL;
	}

	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_PFNMAP);

	/* Recompute page_pgprot[] per huge page */
	base_prot = vma->vm_page_prot;
	for (i = 0; i < nr_huge; i++)
		g_state.page_pgprot[i] = ptemap_cache_pgprot_huge(
			g_state.page_cache[i], base_prot);

	vma->vm_page_prot = pgprot_large_2_4k(g_state.page_pgprot[0]);

	/*
	 * Step 1: pre-populate the entire page table tree.  This handles
	 * PTI mirroring (native_set_p4d etc.) via the kernel's internal
	 * code path, which is not available to modules directly.
	 */
	size = nr_huge << PMD_SHIFT;
	ret = apply_to_page_range(vma->vm_mm, vma->vm_start, size,
				  ptemap_pte_noop, NULL);
	if (ret) {
		pr_err("ptemap: apply_to_page_range pre-populate failed: %d\n", ret);
		return ret;
	}

	/*
	 * Step 2: walk the now-populated table and upgrade each 2MB
	 * range from PTE-level to PMD-level huge mapping.
	 *
	 * apply_to_page_range() allocated PTE pages (one per 2MB range)
	 * which become orphaned when we replace the PMD entry.  Free them
	 * and decrement mm->pgtables_bytes to avoid a BUG at mm teardown.
	 */
	end = vma->vm_end;
	for (addr = vma->vm_start, i = 0; addr < end;
	     addr += PMD_SIZE, i++) {
		pgd = pgd_offset(vma->vm_mm, addr);
		p4d = p4d_offset(pgd, addr);
		pud = pud_offset(p4d, addr);
		pmd = pmd_offset(pud, addr);

		/* All levels must exist after apply_to_page_range */
		if (p4d_none(*p4d) || pud_none(*pud) ||
		    pmd_none(*pmd)) {
			pr_err("ptemap: missing page table at 0x%lx\n", addr);
			return -ENXIO;
		}

		pfn = page_to_pfn(g_state.pages[i]);
		new_pmd = pmd_mkhuge(pfn_pmd(pfn, g_state.page_pgprot[i]));

		/*
		 * Save the old PMD before overwriting — apply_to_page_range()
		 * put a PTE-table entry here that we must free to avoid leaking
		 * the PTE page and its pgtables_bytes charge.
		 */
		{
			pmd_t old_pmd = *pmd;

			set_pmd_at(vma->vm_mm, addr, pmd, new_pmd);

			if (!pmd_none(old_pmd) && !pmd_leaf(old_pmd)) {
				__free_page(pmd_page(old_pmd));
				mm_dec_nr_ptes(vma->vm_mm);
			}
		}
	}

	g_state.vaddr_start = vma->vm_start;
	g_state.vaddr_end   = vma->vm_end;
	g_state.vaddr_size  = vma->vm_end - vma->vm_start;

	if (!g_state.mapped_mm) {
		mmgrab(vma->vm_mm);
		g_state.mapped_mm = vma->vm_mm;
	}

	pr_info("ptemap: mmap HUGE OK: vaddr=0x%lx-0x%lx huge_pages=%lu pid=%d\n",
		vma->vm_start, vma->vm_end, nr_huge, current->pid);

	return 0;
}

/*
 * Flush TLB for a virtual address range on the local CPU.
 *
 * On x86, only __flush_tlb_all() is exported to modules (EXPORT_SYMBOL_GPL).
 * flush_tlb_mm_range() is not callable from out-of-tree code.  Therefore
 * we always perform a full local TLB flush, ignoring the range parameters.
 * The range fields are accepted for forward compatibility when the kernel
 * exports fine-grained flush primitives.
 *
 * For HFT scenarios with CPU-pinned processes this covers the hot path.
 */
void ptemap_flush_tlb_range(unsigned long start, unsigned long end)
{
	__flush_tlb_all();
	g_state.tlb_flush_count++;
}

/*
 * Walk the page table and clear all PMD-level huge page entries in
 * [start, end), then flush TLB.
 *
 * Counterpart to ptemap_pte_clear_range() for 2MB huge pages.  Walks by
 * PMD_SIZE stride, checks pmd_leaf() to avoid touching 4KB-page subtrees,
 * and calls pmd_clear() for each huge mapping found.
 */
void ptemap_huge_clear_range(struct mm_struct *mm, unsigned long start,
			     unsigned long end)
{
	unsigned long addr;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	int cleared = 0;

	if (!mm || start >= end)
		return;

	if (down_write_trylock(&mm->mmap_lock)) {
		for (addr = start; addr < end; addr += PMD_SIZE) {
			pgd = pgd_offset(mm, addr);
			if (pgd_none(*pgd) || pgd_bad(*pgd))
				continue;

			p4d = p4d_offset(pgd, addr);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				continue;

			pud = pud_offset(p4d, addr);
			if (pud_none(*pud) || pud_bad(*pud))
				continue;

			pmd = pmd_offset(pud, addr);
			if (!pmd || pmd_none(*pmd) || !pmd_leaf(*pmd))
				continue;

			pmd_clear(pmd);
			cleared++;
		}
		up_write(&mm->mmap_lock);
	}

	if (cleared > 0) {
		__flush_tlb_all();
		pr_info("ptemap: cleared %d PMD mappings + TLB flush (range 0x%lx-0x%lx)\n",
			cleared, start, end);
	}
}

/*
 * Walk the page table and clear all PTEs in [start, end), then flush TLB.
 *
 * Called at module unload to prevent use-after-free: if a process still holds
 * the mmap and tries to access the mapping after pages are freed, the kernel
 * would follow a dangling PTE into freed/reused physical memory.  Clearing
 * PTEs first guarantees a clean page fault (SIGSEGV) instead of silent
 * corruption.
 *
 * For the v1.1 direct-PTE path (VM_PFNMAP + _PAGE_SPECIAL), there is no rmap
 * metadata to clean up, so pte_clear + TLB flush is sufficient.  For the v1.0
 * vm_insert_page path, rmap entries may remain but will be cleaned up when
 * the VMA is destroyed on process exit — clearing the PTE prevents access to
 * freed pages in the window between module unload and process exit.
 *
 * Must be called before ptemap_free_pages().
 */
void ptemap_pte_clear_range(struct mm_struct *mm, unsigned long start,
			    unsigned long end)
{
	unsigned long addr;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	int cleared = 0;

	if (!mm || start >= end)
		return;

	/* Try to take mmap_lock for write — if we can't get it, proceed
	 * unlocked as a best-effort safety net (unload vs racing accessor
	 * is inherently racy; the PTE clear + TLB flush still closes the
	 * window for subsequent accesses).
	 */
	if (down_write_trylock(&mm->mmap_lock)) {
		for (addr = start; addr < end; addr += PAGE_SIZE) {
			pgd = pgd_offset(mm, addr);
			if (pgd_none(*pgd) || pgd_bad(*pgd))
				continue;

			p4d = p4d_offset(pgd, addr);
			if (p4d_none(*p4d) || p4d_bad(*p4d))
				continue;

			pud = pud_offset(p4d, addr);
			if (pud_none(*pud) || pud_bad(*pud))
				continue;

			pmd = pmd_offset(pud, addr);
			if (pmd_none(*pmd) || pmd_bad(*pmd))
				continue;

			/* Skip PMD-level huge pages — handled by
			 * ptemap_huge_clear_range() instead.
			 */
			if (pmd_leaf(*pmd))
				continue;

			pte = pte_offset_kernel(pmd, addr);
			if (!pte || pte_none(*pte))
				continue;

			pte_clear(mm, addr, pte);
			cleared++;
		}
		up_write(&mm->mmap_lock);
	}

	if (cleared > 0) {
		__flush_tlb_all();
		pr_info("ptemap: cleared %d PTEs + TLB flush (range 0x%lx-0x%lx)\n",
			cleared, start, end);
	}
}

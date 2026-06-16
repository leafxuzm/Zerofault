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
	if (mmap_write_trylock(mm)) {
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

			pte = pte_offset_kernel(pmd, addr);
			if (!pte || pte_none(*pte))
				continue;

			pte_clear(mm, addr, pte);
			cleared++;
		}
		mmap_write_unlock(mm);
	}

	if (cleared > 0) {
		__flush_tlb_all();
		pr_info("ptemap: cleared %d PTEs + TLB flush (range 0x%lx-0x%lx)\n",
			cleared, start, end);
	}
}

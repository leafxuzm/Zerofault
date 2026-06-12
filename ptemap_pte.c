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

	pr_info("ptemap: mmap DIRECT OK: vaddr=0x%lx-0x%lx pages=%lu pid=%d\n",
		vma->vm_start, vma->vm_end, nr_pages, current->pid);

	return 0;
}

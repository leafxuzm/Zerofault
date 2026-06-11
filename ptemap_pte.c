/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ptemap_pte.c — PTE 直写实现（v1.1）
 *
 * 使用 apply_to_page_range() 遍历并填充页表，在回调中通过
 * set_pte_at() + pfn_pte() 直接构造并写入 PTE 条目。
 *
 * 与 v1.0 remap_pfn_range() 方案的关键差异：
 *   - 每页显式控制 PTE bit（pfn_pte + pgprot），不经过 PFN→PTE 的内核默认路径
 *   - apply_to_page_range 在内核内部处理 PGD→P4D→PUD→PMD→PTE 的层级分配
 *   - 回调直接拿到 pte_t *，可做任意 PTE 级别的自定义（v1.2 每页独立 cache 策略）
 *   - 无 rmap 开销（apply_to_page_range 不调用 page_add_file_rmap）
 *
 * 启用方式：insmod ptemap.ko use_direct_pte=1
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/pgtable.h>
#include "ptemap_core.h"

/* PTE 直写回调上下文 */
struct pte_write_ctx {
	struct page **pages;        /* 预分配的物理页数组 */
	unsigned long vma_start;    /* VMA 起始虚拟地址，用于计算页索引 */
	pgprot_t pgprot;            /* 页保护属性（含 cache 策略） */
	struct mm_struct *mm;       /* 目标进程 mm_struct */
};

/*
 * apply_to_page_range 的逐页回调
 *
 * 上下文：apply_to_page_range 已分配好 PTE 页表页 + 持有 ptl spinlock。
 * 本回调拿到的是刚分配/已存在的 pte_t 指针，直接写入 PFN + pgprot。
 *
 * @pte:  pte_t 指针（由 apply_to_page_range 映射并持有锁）
 * @addr: 当前页的起始虚拟地址
 * @data: pte_write_ctx 上下文
 */
static int ptemap_write_pte(pte_t *pte, unsigned long addr, void *data)
{
	struct pte_write_ctx *ctx = data;
	unsigned long idx = (addr - ctx->vma_start) >> PAGE_SHIFT;
	unsigned long pfn = page_to_pfn(ctx->pages[idx]);
	pte_t new_pte;

	new_pte = pfn_pte(pfn, ctx->pgprot);

	/*
	 * set_pte_at: 在 x86_64 上等价于 native_set_pte_at，即
	 *   1. native_set_pte(ptep, pte) — 写入 PTE
	 *   2. __supported_pte_mask 过滤硬件不支持的 bit
	 *
	 * 不触发 rmap/LRU 更新（apply_to_page_range 无此路径）
	 */
	set_pte_at(ctx->mm, addr, pte, new_pte);

	return 0;
}

/*
 * mmap 回调（PTE 直写路径）
 *
 * 替换 v1.0 的 remap_pfn_range() 逐页循环。核心流程：
 *   1. 设置 VMA flags（PFNMAP 告知内核这是原始 PFN 映射）
 *   2. 通过 apply_to_page_range 一次性遍历/填充 VMA 区间内的所有页表
 *   3. 在逐页回调中直接构造 PTE（pfn_pte + pgprot）
 *
 * VMA → apply_to_page_range → [PGD→P4D→PUD→PMD→PTE 层级分配] → 回调写 PTE
 *
 * 调用条件：mmap_write_lock 已由 VFS 层持有（mmap 系统调用路径）
 */
int ptemap_mmap_direct(struct vm_area_struct *vma)
{
	struct pte_write_ctx ctx;
	unsigned long nr_pages;
	int ret;

	nr_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;

	/* PFNMAP: 告知内核不通过 page->mapping 做 rmap 反向查找 */
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP | VM_PFNMAP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	/* 构造回调上下文 */
	ctx.pages     = g_state.pages;
	ctx.vma_start = vma->vm_start;
	ctx.pgprot    = vma->vm_page_prot;
	ctx.mm        = vma->vm_mm;

	/*
	 * apply_to_page_range(mm, addr, size, fn, data):
	 *   - 遍历虚拟地址区间 [addr, addr+size)
	 *   - create=true: 按需分配 PGD→P4D→PUD→PMD→PTE 的中间层级
	 *   - 每页调用 fn(pte, addr, data)，持有 ptl spinlock
	 *   - 不需要调用方手动 mmap 加锁（VFS 已持有 mmap_write_lock）
	 */
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

	pr_info("ptemap: mmap DIRECT PTE write OK: vaddr=0x%lx-0x%lx pages=%lu pid=%d\n",
		vma->vm_start, vma->vm_end, nr_pages, current->pid);

	return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PTEMAP_CORE_H
#define PTEMAP_CORE_H

#include <linux/mm.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>

#define PTEMAP_MAX_PAGES 4096

/* Cache strategy — maps to x86 PAT/PCD/PWT bits */
enum ptemap_cache_mode {
	PTEMAP_CACHE_WC = 0,  /* write-combining (default, PAT bit)   */
	PTEMAP_CACHE_WB = 1,  /* write-back      (clear cache bits)   */
	PTEMAP_CACHE_UC = 2,  /* uncacheable     (PCD+PWT bits)       */
	PTEMAP_CACHE_WT = 3,  /* write-through   (PAT+PWT bits)       */
	PTEMAP_CACHE_NR = 4,
};

/*
 * Convert cache mode enum to pgprot_t.
 * Base prot should come from vma->vm_page_prot.
 */
pgprot_t ptemap_cache_pgprot(enum ptemap_cache_mode mode, pgprot_t base);

/* Global module state */
struct ptemap_state {
	/* Module parameters */
	int phys_pages;
	int target_pid;

	/* Target process */
	struct task_struct *target_task;
	struct mm_struct *target_mm;

	/* Physical pages */
	struct page **pages;
	unsigned long nr_pages;

	/* Per-page cache strategy (indexed by page number) */
	enum ptemap_cache_mode *page_cache;
	pgprot_t *page_pgprot;   /* pre-computed pgprot for each page */

	/* Virtual address range */
	unsigned long vaddr_start;
	unsigned long vaddr_end;
	unsigned long vaddr_size;

	/* cdev */
	dev_t dev_num;
	struct cdev cdev;
	struct class *class;

	/* debugfs */
	struct dentry *debugfs_dir;

	/* PTE direct write */
	int use_direct_pte;

	/* Stats */
	unsigned long alloc_time_ns;
	unsigned long tlb_flush_count;
};

extern struct ptemap_state g_state;

/* ptemap_core.c */
int ptemap_alloc_pages(void);
int ptemap_alloc_cache_arrays(void);
void ptemap_free_cache_arrays(void);
void ptemap_free_pages(void);
void ptemap_free_single_page_range(int start, int nr);

/* ptemap_pte.c — PTE 直写（apply_to_page_range + set_pte_at + pfn_pte） */
int ptemap_mmap_direct(struct vm_area_struct *vma);

/* ptemap_cdev.c */
int ptemap_cdev_init(void);
void ptemap_cdev_exit(void);

/* ptemap_debugfs.c */
int ptemap_debugfs_init(void);
void ptemap_debugfs_exit(void);

#endif /* PTEMAP_CORE_H */

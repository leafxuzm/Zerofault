/* SPDX-License-Identifier: GPL-2.0 */
#ifndef PTEMAP_CORE_H
#define PTEMAP_CORE_H

#include <linux/mm.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/debugfs.h>

#define PTEMAP_MAX_PAGES 4096

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

	/* Stats */
	unsigned long alloc_time_ns;
	unsigned long tlb_flush_count;
};

extern struct ptemap_state g_state;

/* ptemap_core.c */
int ptemap_alloc_pages(void);
int ptemap_build_ptes(void);
void ptemap_clear_ptes(void);
void ptemap_free_pages(void);
void ptemap_free_single_page_range(int start, int nr);

/* ptemap_cdev.c */
int ptemap_cdev_init(void);
void ptemap_cdev_exit(void);

/* ptemap_debugfs.c */
int ptemap_debugfs_init(void);
void ptemap_debugfs_exit(void);

#endif /* PTEMAP_CORE_H */

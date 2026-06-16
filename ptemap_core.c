/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm_types.h>
#include <linux/highmem.h>
#include "ptemap_core.h"

/*
 * Allocate and pin physical pages.
 * Pages persist until ptemap_free_pages() is called at module unload.
 */
int ptemap_alloc_pages(void)
{
	struct page *page;
	int i;

	g_state.pages = kcalloc(g_state.phys_pages, sizeof(struct page *),
				GFP_KERNEL);
	if (!g_state.pages)
		return -ENOMEM;

	for (i = 0; i < g_state.phys_pages; i++) {
		page = alloc_page(GFP_KERNEL);
		if (!page)
			goto rollback;

		/* Pin: prevent swap/compaction from stealing these pages */
		get_page(page);
		/* Prevent RSS counter underflow on unmap (VM_PFNMAP + set_pte_at path) */
		SetPageReserved(page);
		g_state.pages[i] = page;
	}

	g_state.nr_pages = g_state.phys_pages;
	return 0;

rollback:
	ptemap_free_single_page_range(0, i);
	kfree(g_state.pages);
	g_state.pages = NULL;
	return -ENOMEM;
}

void ptemap_free_single_page_range(int start, int nr)
{
	int i;

	for (i = start; i < start + nr && i < g_state.phys_pages; i++) {
		if (g_state.pages[i]) {
			free_reserved_page(g_state.pages[i]);
			g_state.pages[i] = NULL;
		}
	}
}

void ptemap_free_pages(void)
{
	if (!g_state.pages)
		return;

	ptemap_free_single_page_range(0, g_state.nr_pages);
	kfree(g_state.pages);
	g_state.pages = NULL;
	g_state.nr_pages = 0;
}

/*
 * Allocate per-page cache strategy arrays.
 * page_cache[] — enum, user-settable via debugfs
 * page_pgprot[] — pre-computed pgprot_t, used by the mmap callback
 *
 * Default: all pages start as WC (write-combining).
 * Must be called after g_state.phys_pages is set.
 */
int ptemap_alloc_cache_arrays(void)
{
	int i;

	g_state.page_cache = kcalloc(g_state.phys_pages,
			sizeof(enum ptemap_cache_mode), GFP_KERNEL);
	if (!g_state.page_cache)
		return -ENOMEM;

	g_state.page_pgprot = kcalloc(g_state.phys_pages,
			sizeof(pgprot_t), GFP_KERNEL);
	if (!g_state.page_pgprot) {
		kfree(g_state.page_cache);
		g_state.page_cache = NULL;
		return -ENOMEM;
	}

	/* Default all pages to WC */
	for (i = 0; i < g_state.phys_pages; i++) {
		g_state.page_cache[i] = PTEMAP_CACHE_WC;
		g_state.page_pgprot[i] = pgprot_writecombine(PAGE_SHARED);
	}

	pr_info("ptemap: cache arrays allocated (%d pages, default=WC)\n",
		g_state.phys_pages);
	return 0;
}

void ptemap_free_cache_arrays(void)
{
	kfree(g_state.page_cache);
	g_state.page_cache = NULL;
	kfree(g_state.page_pgprot);
	g_state.page_pgprot = NULL;
}

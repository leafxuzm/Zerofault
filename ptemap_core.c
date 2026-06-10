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
			put_page(g_state.pages[i]);
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

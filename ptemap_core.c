/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm_types.h>
#include <linux/highmem.h>
#include <linux/string.h>
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
		if (g_state.huge_page == 2)
			page = alloc_pages_node(g_state.numa_node, GFP_KERNEL | __GFP_COMP,
					   g_state.page_order);
		else
			page = alloc_pages_node(g_state.numa_node, GFP_KERNEL, 0);
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
			if (g_state.huge_page == 2)
				__free_pages(g_state.pages[i],
					     g_state.page_order);
			else
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

/* Look up cache mode enum from name string (case-insensitive) */
int ptemap_cache_mode_from_name(const char *name,
				enum ptemap_cache_mode *mode_out)
{
	static const char * const names[] = {
		[PTEMAP_CACHE_WC] = "WC",
		[PTEMAP_CACHE_WB] = "WB",
		[PTEMAP_CACHE_UC] = "UC",
		[PTEMAP_CACHE_WT] = "WT",
	};
	int i;

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (names[i] && strcasecmp(name, names[i]) == 0) {
			*mode_out = (enum ptemap_cache_mode)i;
			return 0;
		}
	}
	return -EINVAL;
}

/*
 * Allocate per-page cache strategy arrays.
 * page_cache[] — enum, user-settable via debugfs
 * page_pgprot[] — pre-computed pgprot_t, used by the mmap callback
 *
 * Default cache mode from g_state.default_cache_mode (insmod param, WC if unset).
 * Must be called after g_state.phys_pages is set.
 */
int ptemap_alloc_cache_arrays(void)
{
	enum ptemap_cache_mode cache = g_state.default_cache_mode;
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

	/* Default all pages to the configured cache mode */
	for (i = 0; i < g_state.phys_pages; i++) {
		g_state.page_cache[i] = cache;
		g_state.page_pgprot[i] = pgprot_writecombine(PAGE_SHARED);
	}

	pr_info("ptemap: cache arrays allocated (%d pages, default=%s)\n",
		g_state.phys_pages,
		cache == PTEMAP_CACHE_WC ? "WC" :
		cache == PTEMAP_CACHE_WB ? "WB" :
		cache == PTEMAP_CACHE_UC ? "UC" : "WT");
	return 0;
}

void ptemap_free_cache_arrays(void)
{
	kfree(g_state.page_cache);
	g_state.page_cache = NULL;
	kfree(g_state.page_pgprot);
	g_state.page_pgprot = NULL;
}

/*
 * Convert cache mode to PMD-level pgprot_t for 2MB huge pages.
 * Wraps ptemap_cache_pgprot() and shifts _PAGE_PAT from bit 7
 * to bit 12 (_PAGE_PAT_LARGE) via pgprot_4k_2_large().
 *
 * _PAGE_SPECIAL is cleared: it is a PTE-level software bit that
 * is not valid in PMD entries and triggers pmd_bad() warnings.
 */
pgprot_t ptemap_cache_pgprot_huge(enum ptemap_cache_mode mode, pgprot_t base)
{
	pgprot_t prot = ptemap_cache_pgprot(mode, base);
	pgprotval_t val = pgprot_val(prot);

	/* Clear PTE-only bits before promoting to PMD level */
	val &= ~_PAGE_SPECIAL;
	prot = __pgprot(val);

	return pgprot_4k_2_large(prot);
}

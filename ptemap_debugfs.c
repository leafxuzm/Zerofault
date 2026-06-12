/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/mm.h>
#include "ptemap_core.h"

static struct dentry *debugfs_dir;

static const char * const cache_names[] = {
	[PTEMAP_CACHE_WC] = "WC",
	[PTEMAP_CACHE_WB] = "WB",
	[PTEMAP_CACHE_UC] = "UC",
	[PTEMAP_CACHE_WT] = "WT",
};

/* /sys/kernel/debug/ptemap/status */
static int ptemap_status_show(struct seq_file *m, void *v)
{
	seq_printf(m, "state:     LIVE\n");
	seq_printf(m, "version:   1.0.0\n");
	seq_printf(m, "pages:     %lu (total)\n", g_state.nr_pages);
	seq_printf(m, "size:      %lu bytes (%lu MB)\n",
		   g_state.vaddr_size,
		   g_state.vaddr_size / (1024 * 1024));
	seq_printf(m, "target_pid: %d\n", g_state.target_pid);
		seq_printf(m, "direct_pte: %d (%s)\n", g_state.use_direct_pte,
			   g_state.use_direct_pte ? "PTE direct write" : "vm_insert_page");
	seq_printf(m, "vaddr:     0x%lx-0x%lx\n",
		   g_state.vaddr_start, g_state.vaddr_end);
	seq_printf(m, "tlb_flush: %lu\n", g_state.tlb_flush_count);
	return 0;
}

static int ptemap_status_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptemap_status_show, NULL);
}

static const struct file_operations ptemap_status_fops = {
	.owner   = THIS_MODULE,
	.open    = ptemap_status_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* /sys/kernel/debug/ptemap/mappings */
static int ptemap_mappings_show(struct seq_file *m, void *v)
{
	unsigned long i;

	seq_printf(m, "%-5s %-18s %-18s %-10s\n",
		   "idx", "vaddr", "pfn", "size");
	seq_printf(m, "----- ------------------ ------------------ ----------\n");

	if (!g_state.pages) {
		seq_printf(m, "(no pages allocated yet)\n");
		return 0;
	}

	for (i = 0; i < g_state.nr_pages; i++) {
		unsigned long vaddr = 0;
		unsigned long pfn = 0;

		if (g_state.pages[i]) {
			pfn = page_to_pfn(g_state.pages[i]);
		}
		if (g_state.vaddr_start && i < g_state.vaddr_size >> PAGE_SHIFT)
			vaddr = g_state.vaddr_start + (i << PAGE_SHIFT);

		seq_printf(m, "%-5lu 0x%016lx 0x%016lx %-10s\n",
			   i, vaddr, pfn, "4KB");
	}
	return 0;
}

static int ptemap_mappings_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptemap_mappings_show, NULL);
}

static const struct file_operations ptemap_mappings_fops = {
	.owner   = THIS_MODULE,
	.open    = ptemap_mappings_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* /sys/kernel/debug/ptemap/stats */
static int ptemap_stats_show(struct seq_file *m, void *v)
{
	unsigned long total_bytes = g_state.nr_pages * PAGE_SIZE;

	seq_printf(m, "total_bytes:    %lu (%lu MB)\n",
		   total_bytes, total_bytes / (1024 * 1024));
	seq_printf(m, "nr_pages:       %lu\n", g_state.nr_pages);
	seq_printf(m, "page_size:      %lu\n", PAGE_SIZE);
	seq_printf(m, "tlb_flush_count: %lu\n", g_state.tlb_flush_count);
	return 0;
}

static int ptemap_stats_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptemap_stats_show, NULL);
}

static const struct file_operations ptemap_stats_fops = {
	.owner   = THIS_MODULE,
	.open    = ptemap_stats_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

/* /sys/kernel/debug/ptemap/cache_policy (rw) */
static int ptemap_cache_show(struct seq_file *m, void *v)
{
	unsigned long i;
	int count[PTEMAP_CACHE_NR] = {0};

	if (!g_state.page_cache) {
		seq_printf(m, "(no cache arrays)\n");
		return 0;
	}

	for (i = 0; i < g_state.nr_pages; i++)
		count[g_state.page_cache[i]]++;

	seq_printf(m, "%-5s %-6s %s\n", "pages", "count", "mode");
	seq_printf(m, "----- ------ ------\n");
	for (i = 0; i < PTEMAP_CACHE_NR; i++) {
		if (count[i] > 0)
			seq_printf(m, "%-5s %-6d %s\n",
				   i == PTEMAP_CACHE_WC ? "all" :
				   (cache_names[i] + 1), /* skip the '? ' */
				   count[i], cache_names[i]);
	}

	/* Detail: show per-page policy for ranges */
	seq_printf(m, "\n--- per-page detail ---\n");
	seq_printf(m, "%-6s %-4s\n", "range", "mode");
	seq_printf(m, "------ ----\n");

	if (g_state.nr_pages > 0) {
		int start = 0;
		enum ptemap_cache_mode cur = g_state.page_cache[0];

		for (i = 1; i <= g_state.nr_pages; i++) {
			if (i == g_state.nr_pages ||
			    g_state.page_cache[i] != cur) {
				if (i - 1 == start)
					seq_printf(m, "%-6d %-4s\n",
						   start, cache_names[cur]);
				else
					seq_printf(m, "%-6d-%-3lu %-4s\n",
						   start, i - 1,
						   cache_names[cur]);
				if (i < g_state.nr_pages) {
					start = i;
					cur = g_state.page_cache[i];
				}
			}
		}
	}
	return 0;
}

static int ptemap_cache_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, ptemap_cache_show, NULL);
}

/*
 * Parse "start[-end] MODE" — set cache policy for a page or range.
 *
 * Examples:
 *   echo "0-63 WC"  > cache_policy   # pages 0-63: write-combining
 *   echo "64 WB"    > cache_policy   # page 64: write-back
 *   echo "128-255 UC" > cache_policy # pages 128-255: uncacheable
 */
static ssize_t ptemap_cache_write(struct file *filp, const char __user *buf,
				  size_t len, loff_t *off)
{
	char kbuf[64];
	unsigned long start, end;
	char mode_str[8];
	int mode;

	if (len >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;
	kbuf[len] = '\0';

	/* Parse */
	if (sscanf(kbuf, "%lu-%lu %7s", &start, &end, mode_str) == 3) {
		/* range: start-end MODE */
	} else if (sscanf(kbuf, "%lu %7s", &start, mode_str) == 2) {
		end = start;  /* single page */
	} else {
		return -EINVAL;
	}

	if (end < start || end >= g_state.nr_pages)
		return -ERANGE;

	/* Look up mode string */
	for (mode = 0; mode < PTEMAP_CACHE_NR; mode++) {
		if (strcasecmp(mode_str, cache_names[mode]) == 0)
			break;
	}
	if (mode == PTEMAP_CACHE_NR)
		return -EINVAL;

	/* Set cache strategy for the range */
	for (; start <= end; start++)
		g_state.page_cache[start] = (enum ptemap_cache_mode)mode;

	pr_info("ptemap: cache_policy updated (range → %s)\n",
		cache_names[mode]);

	return len;
}

static const struct file_operations ptemap_cache_fops = {
	.owner   = THIS_MODULE,
	.open    = ptemap_cache_open,
	.read    = seq_read,
	.write   = ptemap_cache_write,
	.llseek  = seq_lseek,
	.release = single_release,
};

int ptemap_debugfs_init(void)
{
	debugfs_dir = debugfs_create_dir("ptemap", NULL);
	if (IS_ERR(debugfs_dir)) {
		pr_err("ptemap: debugfs_create_dir failed\n");
		return PTR_ERR(debugfs_dir);
	}

	debugfs_create_file("status", 0444, debugfs_dir, NULL,
			    &ptemap_status_fops);
	debugfs_create_file("mappings", 0444, debugfs_dir, NULL,
			    &ptemap_mappings_fops);
	debugfs_create_file("stats", 0444, debugfs_dir, NULL,
			    &ptemap_stats_fops);
	debugfs_create_file("cache_policy", 0644, debugfs_dir, NULL,
			    &ptemap_cache_fops);

	pr_info("ptemap: debugfs at /sys/kernel/debug/ptemap/\n");
	return 0;
}

void ptemap_debugfs_exit(void)
{
	if (debugfs_dir)
		debugfs_remove_recursive(debugfs_dir);
	debugfs_dir = NULL;
}

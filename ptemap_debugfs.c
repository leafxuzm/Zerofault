/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mm.h>
#include "ptemap_core.h"

static struct dentry *debugfs_dir;

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

	pr_info("ptemap: debugfs at /sys/kernel/debug/ptemap/\n");
	return 0;
}

void ptemap_debugfs_exit(void)
{
	if (debugfs_dir)
		debugfs_remove_recursive(debugfs_dir);
	debugfs_dir = NULL;
}

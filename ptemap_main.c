/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include "ptemap_core.h"

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("PTE direct-mapping module for HFT low-latency memory access");
MODULE_AUTHOR("Leaf Xu");
MODULE_VERSION("1.0.0");

/* Module parameters */
static int phys_pages = 256;
module_param(phys_pages, int, 0644);
MODULE_PARM_DESC(phys_pages, "Number of 4KB physical pages to allocate (default: 256 = 1MB)");

static int target_pid;
module_param(target_pid, int, 0644);
MODULE_PARM_DESC(target_pid, "Target process PID for access control (0 = any process, default: 0)");

static int use_direct_pte;
module_param(use_direct_pte, int, 0644);
MODULE_PARM_DESC(use_direct_pte, "Use PTE direct write (1) instead of vm_insert_page (0, default)");

/* Global module state */
struct ptemap_state g_state = {
	.phys_pages = 256,
	.target_pid = 0,
};

static int __init ptemap_init(void)
{
	int ret;

	pr_info("ptemap: loading v1.0.0\n");

	/* [1] Validate and store parameters */
	if (phys_pages <= 0 || phys_pages > PTEMAP_MAX_PAGES) {
		pr_err("ptemap: phys_pages=%d out of range (1-%d)\n",
		       phys_pages, PTEMAP_MAX_PAGES);
		return -EINVAL;
	}
	g_state.phys_pages = phys_pages;
	g_state.target_pid = target_pid;
	g_state.use_direct_pte = use_direct_pte;

	/* [2] If target_pid specified, verify process exists */
	if (target_pid > 0) {
		struct task_struct *task;

		rcu_read_lock();
		task = pid_task(find_vpid(target_pid), PIDTYPE_PID);
		if (task)
			get_task_struct(task);
		rcu_read_unlock();

		if (!task) {
			pr_err("ptemap: process PID %d not found\n", target_pid);
			return -ESRCH;
		}
		if (!task->mm) {
			pr_err("ptemap: PID %d is a kernel thread, no user memory\n",
			       target_pid);
			put_task_struct(task);
			return -EINVAL;
		}
		g_state.target_task = task;
		g_state.target_mm = get_task_mm(task);
		if (!g_state.target_mm) {
			put_task_struct(task);
			g_state.target_task = NULL;
			return -EINVAL;
		}
	}

	/* [3] Allocate physical pages */
	pr_info("ptemap: allocating %d pages (%lu KB)...\n",
		g_state.phys_pages,
		(g_state.phys_pages * PAGE_SIZE) / 1024);

	ret = ptemap_alloc_pages();
	if (ret) {
		pr_err("ptemap: page allocation failed\n");
		goto err_pages;
	}
	pr_info("ptemap: allocated %lu pages OK\n", g_state.nr_pages);

	/* [3.5] Allocate per-page cache strategy arrays */
	ret = ptemap_alloc_cache_arrays();
	if (ret) {
		pr_err("ptemap: cache array allocation failed\n");
		goto err_cache;
	}

	/* [4] Register cdev */
	ret = ptemap_cdev_init();
	if (ret) {
		pr_err("ptemap: cdev registration failed\n");
		goto err_cdev;
	}

	/* [5] Create debugfs */
	ret = ptemap_debugfs_init();
	if (ret) {
		pr_err("ptemap: debugfs creation failed (non-fatal)\n");
		/* debugfs is optional, continue */
	}

	pr_info("ptemap: loaded OK (pages=%lu, target_pid=%d, state=LIVE)\n",
		g_state.nr_pages, g_state.target_pid);
	return 0;

err_cdev:
	ptemap_free_cache_arrays();
err_cache:
	ptemap_free_pages();
err_pages:
	if (g_state.target_mm) {
		mmput(g_state.target_mm);
		g_state.target_mm = NULL;
	}
	if (g_state.target_task) {
		put_task_struct(g_state.target_task);
		g_state.target_task = NULL;
	}
	return ret;
}

static void __exit ptemap_exit(void)
{
	pr_info("ptemap: unloading...\n");

	/* [1] Remove debugfs */
	ptemap_debugfs_exit();

	/* [2] Unregister cdev */
	ptemap_cdev_exit();

	/* [3] Release mm and task references */
	if (g_state.target_mm) {
		mmput(g_state.target_mm);
		g_state.target_mm = NULL;
	}
	if (g_state.target_task) {
		put_task_struct(g_state.target_task);
		g_state.target_task = NULL;
	}

	/* [3.5] Free cache strategy arrays */
	ptemap_free_cache_arrays();

	/* [4] Free physical pages */
	ptemap_free_pages();

	pr_info("ptemap: unloaded OK\n");
}

module_init(ptemap_init);
module_exit(ptemap_exit);

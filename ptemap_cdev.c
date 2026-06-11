/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/capability.h>
#include "ptemap_core.h"

#define PTEMAP_DEV_NAME "ptemap"

static int ptemap_open(struct inode *inode, struct file *filp)
{
	/* Access control: only target_pid process (or any if target_pid=0) */
	if (g_state.target_pid > 0 && current->pid != g_state.target_pid) {
		pr_warn("ptemap: PID %d (%s) denied access (target_pid=%d)\n",
			current->pid, current->comm, g_state.target_pid);
		return -EPERM;
	}

	/* Use module state as file private data */
	filp->private_data = &g_state;
	return 0;
}

static int ptemap_release(struct inode *inode, struct file *filp)
{
	filp->private_data = NULL;
	return 0;
}

/*
 * mmap callback: map pre-allocated physical pages into the calling
 * process's virtual address space. All pages were allocated at module
 * init time - no allocation happens here, so no page faults at runtime.
 */
static int ptemap_mmap(struct file *filp, struct vm_area_struct *vma)
{
	unsigned long i;
	unsigned long size = vma->vm_end - vma->vm_start;
	unsigned long nr_requested = size >> PAGE_SHIFT;

	if (!g_state.pages || g_state.nr_pages == 0) {
		pr_err("ptemap: no pages available for mmap\n");
		return -ENOMEM;
	}

	if (nr_requested > g_state.nr_pages) {
		pr_err("ptemap: requested %lu pages, only %lu available\n",
		       nr_requested, g_state.nr_pages);
		return -EINVAL;
	}

	/*
	 * Set VMA flags: IO prevents swapping, DONTEXPAND prevents
	 * mremap from moving the mapping, DONTDUMP excludes from
	 * core dumps (performance-sensitive memory).
	 */
	/* v1.1: PTE 直写路径 — apply_to_page_range + set_pte_at，完全绕过
	 * vm_insert_page / remap_pfn_range，零 rmap 开销，逐页可独立 pgprot */
	if (g_state.use_direct_pte)
		return ptemap_mmap_direct(vma);

	/* v1.0: vm_insert_page 路径（默认） */
	vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);

	for (i = 0; i < nr_requested; i++) {
		unsigned long vaddr = vma->vm_start + (i << PAGE_SHIFT);
		int ret;

		ret = vm_insert_page(vma, vaddr, g_state.pages[i]);
		if (ret) {
			pr_err("ptemap: vm_insert_page failed at vaddr=0x%lx (page %lu): %d\n",
			       vaddr, i, ret);
			return ret;
		}
	}

	pr_info("ptemap: mmap OK: vaddr=0x%lx-0x%lx pages=%lu pid=%d\n",
		vma->vm_start, vma->vm_end, nr_requested, current->pid);

	g_state.vaddr_start = vma->vm_start;
	g_state.vaddr_end = vma->vm_end;
	g_state.vaddr_size = size;

	return 0;
}

static long ptemap_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	/* Reserved for v1.1: query mapping info, stats */
	return -ENOTTY;
}

static const struct file_operations ptemap_fops = {
	.owner          = THIS_MODULE,
	.open           = ptemap_open,
	.release        = ptemap_release,
	.mmap           = ptemap_mmap,
	.unlocked_ioctl = ptemap_ioctl,
};

int ptemap_cdev_init(void)
{
	int ret;

	ret = alloc_chrdev_region(&g_state.dev_num, 0, 1, PTEMAP_DEV_NAME);
	if (ret) {
		pr_err("ptemap: alloc_chrdev_region failed: %d\n", ret);
		return ret;
	}

	cdev_init(&g_state.cdev, &ptemap_fops);
	g_state.cdev.owner = THIS_MODULE;

	ret = cdev_add(&g_state.cdev, g_state.dev_num, 1);
	if (ret) {
		pr_err("ptemap: cdev_add failed: %d\n", ret);
		unregister_chrdev_region(g_state.dev_num, 1);
		return ret;
	}

	/* Create device node automatically */
	g_state.class = class_create(PTEMAP_DEV_NAME);
	if (IS_ERR(g_state.class)) {
		pr_err("ptemap: class_create failed\n");
		cdev_del(&g_state.cdev);
		unregister_chrdev_region(g_state.dev_num, 1);
		return PTR_ERR(g_state.class);
	}

	if (IS_ERR(device_create(g_state.class, NULL, g_state.dev_num,
				 NULL, PTEMAP_DEV_NAME))) {
		pr_err("ptemap: device_create failed\n");
		class_destroy(g_state.class);
		cdev_del(&g_state.cdev);
		unregister_chrdev_region(g_state.dev_num, 1);
		return -ENODEV;
	}

	pr_info("ptemap: /dev/%s registered (major=%d)\n",
		PTEMAP_DEV_NAME, MAJOR(g_state.dev_num));
	return 0;
}

void ptemap_cdev_exit(void)
{
	device_destroy(g_state.class, g_state.dev_num);
	class_destroy(g_state.class);
	cdev_del(&g_state.cdev);
	unregister_chrdev_region(g_state.dev_num, 1);
	pr_info("ptemap: /dev/%s unregistered\n", PTEMAP_DEV_NAME);
}

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
#include <linux/mm_types.h>
#include "ptemap_core.h"
#include "ptemap.h"

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
	unsigned long nr_requested = size / g_state.page_size;

	if (!g_state.pages || g_state.nr_pages == 0) {
		pr_err("ptemap: no pages available for mmap\n");
		return -ENOMEM;
	}

	if (nr_requested > g_state.nr_pages) {
		pr_err("ptemap: requested %lu pages (%lu KB), only %lu available\n",
		       nr_requested, size / 1024, g_state.nr_pages);
		return -EINVAL;
	}

	/* v1.4: 2MB huge page path — manual PGD→P4D→PUD→PMD walk */
	if (g_state.huge_page == 2)
		return ptemap_mmap_huge(vma);

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

	/* Track the mm for safe PTE cleanup at module unload */
	if (!g_state.mapped_mm) {
		mmgrab(vma->vm_mm);
		g_state.mapped_mm = vma->vm_mm;
	}

	return 0;
}

/*
 * PTEMAP_IOC_QUERY — query a single page by index.
 */
static int ptemap_ioctl_query(unsigned long arg)
{
	struct ptemap_query_req req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.page_idx >= g_state.nr_pages)
		return -EINVAL;

	/* PFN */
	if (g_state.pages && g_state.pages[req.page_idx])
		req.pfn = page_to_pfn(g_state.pages[req.page_idx]);
	else
		req.pfn = 0;

	/* VA */
	if (g_state.vaddr_start)
		req.vaddr = g_state.vaddr_start + (unsigned long)req.page_idx * g_state.page_size;
	else
		req.vaddr = 0;

	/* Cache mode */
	if (g_state.page_cache)
		req.cache_mode = g_state.page_cache[req.page_idx];
	else
		req.cache_mode = PTEMAP_CACHE_WC;

	/* Page size */
	req.page_size = g_state.page_size;

	if (copy_to_user((void __user *)arg, &req, sizeof(req)))
		return -EFAULT;

	return 0;
}

/*
 * PTEMAP_IOC_QUERY_RANGE — batch query a page range.
 */
static int ptemap_ioctl_query_range(unsigned long arg)
{
	struct ptemap_query_range_req req;
	unsigned long i, end;
	__u32 *cache_kern = NULL;
	__u64 *pfn_kern = NULL;
	__u64 *va_kern = NULL;
	int ret = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	if (req.start_idx >= g_state.nr_pages ||
	    req.nr_pages == 0 ||
	    req.nr_pages > g_state.nr_pages - req.start_idx)
		return -EINVAL;

	end = req.start_idx + req.nr_pages;

	/* Validate user pointers */
	if (!req.pfn_buf || !req.vaddr_buf || !req.cache_buf)
		return -EINVAL;

	/* Allocate kernel-side temp buffers */
	cache_kern = kmalloc_array(req.nr_pages, sizeof(__u32), GFP_KERNEL);
	pfn_kern   = kmalloc_array(req.nr_pages, sizeof(__u64), GFP_KERNEL);
	va_kern    = kmalloc_array(req.nr_pages, sizeof(__u64), GFP_KERNEL);
	if (!cache_kern || !pfn_kern || !va_kern) {
		ret = -ENOMEM;
		goto out;
	}

	for (i = req.start_idx; i < end; i++) {
		unsigned long idx = i - req.start_idx;

		pfn_kern[idx] = (g_state.pages && g_state.pages[i])
			? page_to_pfn(g_state.pages[i]) : 0;

		va_kern[idx] = g_state.vaddr_start
			? g_state.vaddr_start + (i * g_state.page_size) : 0;

		cache_kern[idx] = g_state.page_cache
			? g_state.page_cache[i] : PTEMAP_CACHE_WC;
	}

	if (copy_to_user(req.pfn_buf,   pfn_kern,   req.nr_pages * sizeof(__u64)))
		{ ret = -EFAULT; goto out; }
	if (copy_to_user(req.vaddr_buf,  va_kern,    req.nr_pages * sizeof(__u64)))
		{ ret = -EFAULT; goto out; }
	if (copy_to_user(req.cache_buf,  cache_kern, req.nr_pages * sizeof(__u32)))
		{ ret = -EFAULT; goto out; }

out:
	kfree(cache_kern);
	kfree(pfn_kern);
	kfree(va_kern);
	return ret;
}

/*
 * PTEMAP_IOC_FLUSH_TLB — full local TLB flush.
 */
static int ptemap_ioctl_flush_tlb(void)
{
	ptemap_flush_tlb_range(0, TLB_FLUSH_ALL);
	return 0;
}

/*
 * PTEMAP_IOC_FLUSH_TLB_RANGE — range-directed TLB flush.
 * On current x86 the flush is always full-TLB (see ptemap_pte.c).
 */
static int ptemap_ioctl_flush_tlb_range(unsigned long arg)
{
	struct ptemap_flush_req req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	ptemap_flush_tlb_range(req.start, req.start + req.len);
	return 0;
}

static long ptemap_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case PTEMAP_IOC_QUERY:
		return ptemap_ioctl_query(arg);
	case PTEMAP_IOC_QUERY_RANGE:
		return ptemap_ioctl_query_range(arg);
	case PTEMAP_IOC_FLUSH_TLB:
		return ptemap_ioctl_flush_tlb();
	case PTEMAP_IOC_FLUSH_TLB_RANGE:
		return ptemap_ioctl_flush_tlb_range(arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations ptemap_fops = {
	.owner          = THIS_MODULE,
	.open           = ptemap_open,
	.release        = ptemap_release,
	.mmap           = ptemap_mmap,
	.unlocked_ioctl = ptemap_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
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

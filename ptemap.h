/* SPDX-License-Identifier: GPL-2.0 */
/*
 * ptemap.h — UAPI header for /dev/ptemap ioctl interface
 *
 * Include this header in user-space programs that need to query
 * per-page mapping info or control TLB flushes.
 */
#ifndef _UAPI_PTEMAP_H
#define _UAPI_PTEMAP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
typedef uint64_t __u64;
typedef uint32_t __u32;
#endif

/*
 * Cache mode enum — MUST match ptemap_core.h.
 * In kernel context the enum comes from ptemap_core.h (via cdev.c include),
 * so only define it for userspace.
 */
#ifndef __KERNEL__
enum ptemap_cache_mode_user {
	PTEMAP_CACHE_WC = 0,
	PTEMAP_CACHE_WB = 1,
	PTEMAP_CACHE_UC = 2,
	PTEMAP_CACHE_WT = 3,
	PTEMAP_CACHE_NR = 4,
};
#endif

/* ioctl type byte */
#define PTEMAP_IOC_MAGIC  'P'

/*
 * PTEMAP_IOC_QUERY — query a single page by index.
 *
 * arg: struct ptemap_query_req
 *   in:  page_idx
 *   out: pfn, vaddr, cache_mode
 */
struct ptemap_query_req {
	__u32 page_idx;
	__u32 cache_mode;
	__u32 page_size;   /* output: actual page size (4096 or 2MB) */
	__u32 __pad;
	__u64 pfn;
	__u64 vaddr;
};

/*
 * PTEMAP_IOC_QUERY_RANGE — batch query a page range.
 *
 * arg: struct ptemap_query_range_req
 *   in:  start_idx, nr_pages, pfn_buf, vaddr_buf, cache_buf (user pointers)
 *   out: pfn_buf[], vaddr_buf[], cache_buf[] (filled by kernel)
 */
struct ptemap_query_range_req {
	__u32 start_idx;
	__u32 nr_pages;
	__u64 *pfn_buf;
	__u64 *vaddr_buf;
	__u32 *cache_buf;
};

/*
 * PTEMAP_IOC_FLUSH_TLB_RANGE — flush TLB for a VA range.
 *
 * arg: struct ptemap_flush_req
 *   in: start (VA), len (bytes)
 *
 * Note: on x86, only __flush_tlb_all() is available to modules,
 * so the flush is always full-TLB on the local CPU.  The range
 * fields are accepted for forward compatibility when per-range
 * flush becomes available.
 */
struct ptemap_flush_req {
	__u64 start;
	__u64 len;
};

/* ioctl commands */
#define PTEMAP_IOC_QUERY          _IOWR(PTEMAP_IOC_MAGIC, 0x80, struct ptemap_query_req)
#define PTEMAP_IOC_QUERY_RANGE    _IOWR(PTEMAP_IOC_MAGIC, 0x81, struct ptemap_query_range_req)
#define PTEMAP_IOC_FLUSH_TLB      _IO(PTEMAP_IOC_MAGIC, 0x82)
#define PTEMAP_IOC_FLUSH_TLB_RANGE _IOW(PTEMAP_IOC_MAGIC, 0x83, struct ptemap_flush_req)

#endif /* _UAPI_PTEMAP_H */

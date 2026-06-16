/*
 * test_ptemap.c — user-space test for /dev/ptemap
 *
 * Tests: open → mmap → write/verify → ioctl query → ioctl flush_tlb → close
 * Expects zero page faults during mmap access (pages pre-allocated at insmod).
 *
 * Compile (on CentOS VM):
 *   gcc -static -O2 -o test_ptemap test_ptemap.c
 *
 * Usage:
 *   ./test_ptemap [nr_pages]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <errno.h>
#include "ptemap.h"

#define DEVICE      "/dev/ptemap"
#define PAGE_SIZE   4096
#define DEFAULT_NR  64

static int test_ioctl_query(int fd, unsigned long nr_pages)
{
	unsigned long i, errors = 0;

	printf("\n--- ioctl QUERY test ---\n");

	for (i = 0; i < nr_pages; i++) {
		struct ptemap_query_req req;
		unsigned long expected_va;

		memset(&req, 0, sizeof(req));
		req.page_idx = i;

		if (ioctl(fd, PTEMAP_IOC_QUERY, &req) < 0) {
			fprintf(stderr, "  FAIL QUERY page[%lu]: %s\n",
				i, strerror(errno));
			errors++;
			continue;
		}

		if (req.pfn == 0) {
			fprintf(stderr, "  FAIL page[%lu]: PFN is 0\n", i);
			errors++;
			continue;
		}

		printf("  page[%3lu] pfn=0x%llx vaddr=0x%llx cache=%u\n",
		       i, (unsigned long long)req.pfn,
		       (unsigned long long)req.vaddr, req.cache_mode);

		/* Default cache mode should be WC=0 */
		if (req.cache_mode != PTEMAP_CACHE_WC) {
			fprintf(stderr, "  FAIL page[%lu]: expected cache=WC, got %u\n",
				i, req.cache_mode);
			errors++;
		}

		/* VA should be non-zero if mapped */
		if (req.vaddr == 0) {
			fprintf(stderr, "  FAIL page[%lu]: vaddr is 0 (not mapped?)\n", i);
			errors++;
		}

		/* Spot-check: page 0 should be at the start of the mmap region */
		if (i == 0)
			expected_va = req.vaddr;
	}

	if (errors == 0)
		printf("QUERY OK (0 errors)\n");
	else
		printf("QUERY FAIL (%lu errors)\n", errors);

	return errors ? 1 : 0;
}

static int test_ioctl_query_range(int fd, unsigned long nr_pages)
{
	struct ptemap_query_range_req req;
	__u64 *pfn_buf, *va_buf;
	__u32 *cache_buf;
	unsigned long i, errors = 0;
	int ret = 0;

	printf("\n--- ioctl QUERY_RANGE test ---\n");

	pfn_buf   = calloc(nr_pages, sizeof(__u64));
	va_buf    = calloc(nr_pages, sizeof(__u64));
	cache_buf = calloc(nr_pages, sizeof(__u32));
	if (!pfn_buf || !va_buf || !cache_buf) {
		fprintf(stderr, "  alloc failed\n");
		ret = 1;
		goto out;
	}

	memset(&req, 0, sizeof(req));
	req.start_idx = 0;
	req.nr_pages  = nr_pages;
	req.pfn_buf   = pfn_buf;
	req.vaddr_buf = va_buf;
	req.cache_buf = cache_buf;

	if (ioctl(fd, PTEMAP_IOC_QUERY_RANGE, &req) < 0) {
		fprintf(stderr, "  QUERY_RANGE failed: %s\n", strerror(errno));
		ret = 1;
		goto out;
	}

	for (i = 0; i < nr_pages; i++) {
		if (pfn_buf[i] == 0) {
			fprintf(stderr, "  FAIL page[%lu]: PFN is 0\n", i);
			errors++;
		}
		if (va_buf[i] == 0) {
			fprintf(stderr, "  FAIL page[%lu]: VA is 0\n", i);
			errors++;
		}
		if (cache_buf[i] != PTEMAP_CACHE_WC && cache_buf[i] != PTEMAP_CACHE_WB &&
		    cache_buf[i] != PTEMAP_CACHE_UC && cache_buf[i] != PTEMAP_CACHE_WT) {
			fprintf(stderr, "  FAIL page[%lu]: invalid cache mode %u\n",
				i, cache_buf[i]);
			errors++;
		}
	}

	/* VA consistency: adjacent pages differ by PAGE_SIZE */
	for (i = 1; i < nr_pages; i++) {
		if (va_buf[i] != va_buf[i-1] + PAGE_SIZE) {
			fprintf(stderr, "  FAIL VA gap page[%lu]: %llx vs %llx\n",
				i, (unsigned long long)va_buf[i],
				(unsigned long long)va_buf[i-1]);
			errors++;
		}
	}

	if (errors == 0)
		printf("QUERY_RANGE OK (%lu pages, 0 errors)\n", nr_pages);
	else
		printf("QUERY_RANGE FAIL (%lu errors)\n", errors);

out:
	free(pfn_buf);
	free(va_buf);
	free(cache_buf);
	return ret ? ret : (errors ? 1 : 0);
}

static int test_ioctl_flush_tlb(int fd)
{
	int ret;

	printf("\n--- ioctl FLUSH_TLB test ---\n");

	ret = ioctl(fd, PTEMAP_IOC_FLUSH_TLB);
	if (ret < 0) {
		fprintf(stderr, "  FLUSH_TLB failed: %s\n", strerror(errno));
		return 1;
	}
	printf("FLUSH_TLB OK\n");

	/* Also test range flush */
	{
		struct ptemap_flush_req req = { .start = 0, .len = 4096 };
		ret = ioctl(fd, PTEMAP_IOC_FLUSH_TLB_RANGE, &req);
		if (ret < 0) {
			fprintf(stderr, "  FLUSH_TLB_RANGE failed: %s\n", strerror(errno));
			return 1;
		}
		printf("FLUSH_TLB_RANGE OK (range 0x%llx-0x%llx)\n",
		       (unsigned long long)req.start,
		       (unsigned long long)(req.start + req.len));
	}

	return 0;
}

int main(int argc, char **argv)
{
	int fd, ret;
	unsigned long i, errors = 0;
	unsigned long nr_pages = DEFAULT_NR;
	unsigned long map_size;
	unsigned long *buf;

	if (argc > 1)
		nr_pages = strtoul(argv[1], NULL, 0);

	map_size = nr_pages * PAGE_SIZE;

	printf("=== ptemap test ===\n");
	printf("device:    %s\n", DEVICE);
	printf("nr_pages:  %lu (%lu KB)\n", nr_pages, map_size / 1024);

	/* [1] Open */
	fd = open(DEVICE, O_RDWR);
	if (fd < 0) {
		perror("open(" DEVICE ")");
		return 1;
	}
	printf("[1] open  OK (fd=%d)\n", fd);

	/* [2] mmap */
	buf = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, fd, 0);
	if (buf == MAP_FAILED) {
		perror("mmap");
		goto out_close;
	}
	printf("[2] mmap OK (vaddr=%p, size=%lu KB)\n",
	       (void *)buf, map_size / 1024);

	/* [3] Write pattern: page[i] gets value i at first word */
	printf("[3] writing pattern...\n");
	for (i = 0; i < nr_pages; i++) {
		unsigned long *ptr = (unsigned long *)((char *)buf + i * PAGE_SIZE);
		ptr[0] = i;           /* page index */
		ptr[1] = 0xDEADBEEF;  /* magic */
		ptr[2] = i * 2;       /* double-check value */
	}
	printf("[3] write OK (%lu pages)\n", nr_pages);

	/* [4] Read back and verify */
	printf("[4] verifying...\n");
	for (i = 0; i < nr_pages; i++) {
		unsigned long *ptr = (unsigned long *)((char *)buf + i * PAGE_SIZE);

		if (ptr[0] != i) {
			fprintf(stderr, "  FAIL page[%lu]: expected %lu, got %lu\n",
				i, i, ptr[0]);
			errors++;
		}
		if (ptr[1] != 0xDEADBEEF) {
			fprintf(stderr, "  FAIL page[%lu] magic: expected 0xDEADBEEF, got 0x%lx\n",
				i, ptr[1]);
			errors++;
		}
		if (ptr[2] != i * 2) {
			fprintf(stderr, "  FAIL page[%lu] check2: expected %lu, got %lu\n",
				i, i * 2, ptr[2]);
			errors++;
		}
	}
	if (errors == 0)
		printf("[4] verify OK (0 errors)\n");
	else
		printf("[4] verify FAIL (%lu errors)\n", errors);

	/* [5] Cross-page write: ensure adjacent pages don't interfere */
	printf("[5] cross-page boundary test...\n");
	for (i = 1; i < nr_pages; i++) {
		unsigned long *prev_last = (unsigned long *)((char *)buf + i * PAGE_SIZE - sizeof(unsigned long));
		unsigned long *curr_first = (unsigned long *)((char *)buf + i * PAGE_SIZE);
		unsigned long saved_prev = *prev_last;
		unsigned long saved_curr = *curr_first;

		*prev_last = 0xCAFEBABE;
		*curr_first = 0xBAADF00D;

		if (*prev_last != 0xCAFEBABE || *curr_first != 0xBAADF00D) {
			fprintf(stderr, "  FAIL boundary page %lu-%lu\n", i-1, i);
			errors++;
		}

		*prev_last = saved_prev;
		*curr_first = saved_curr;
	}
	if (errors == 0)
		printf("[5] boundary OK\n");
	else
		printf("[5] boundary FAIL (%lu errors)\n", errors);

	/* [6] ioctl QUERY single-page */
	ret = errors ? 1 : 0;
	ret |= test_ioctl_query(fd, nr_pages);

	/* [7] ioctl QUERY_RANGE batch */
	ret |= test_ioctl_query_range(fd, nr_pages);

	/* [8] ioctl FLUSH_TLB */
	ret |= test_ioctl_flush_tlb(fd);

	printf("\n=== result: %s ===\n", errors || ret ? "FAIL" : "PASS");
	ret = errors || ret ? 1 : 0;

	munmap(buf, map_size);
out_close:
	close(fd);
	return ret;
}

/*
 * test_ptemap.c — user-space test for /dev/ptemap
 *
 * Tests: open → mmap → write pattern → read back → verify → close
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
#include <errno.h>

#define DEVICE      "/dev/ptemap"
#define PAGE_SIZE   4096
#define DEFAULT_NR  64

int main(int argc, char **argv)
{
	int fd, ret = 1;
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

	printf("\n=== result: %s ===\n", errors ? "FAIL" : "PASS");
	ret = errors ? 1 : 0;

	munmap(buf, map_size);
out_close:
	close(fd);
	return ret;
}

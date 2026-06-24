/* SPDX-License-Identifier: GPL-2.0 */
/*
 * bench_ptemap.c — Performance benchmark for Zerofault vs traditional mmap
 *
 * Build: gcc -static -O2 -o bench_ptemap bench_ptemap.c
 *
 * Measures: mmap latency, first-access latency, read/write throughput,
 *           ioctl latency, cache mode impact.
 *
 * Compares: Zerofault (direct PTE / vm_insert_page / huge page)
 *           vs MAP_ANONYMOUS vs MAP_POPULATE
 *
 * No perf required — page fault counting via /proc/self/stat field 10.
 */

#define _GNU_SOURCE    /* for MAP_POPULATE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <time.h>
#include <errno.h>
#include "ptemap.h"

/* --- constants --- */
#define CACHE_POLICY_PATH "/sys/kernel/debug/ptemap/cache_policy"
#define STATS_PATH        "/sys/kernel/debug/ptemap/stats"

#define DEFAULT_NR_PAGES  256
#define DEFAULT_ITER      11     /* 1 warmup + 10 measured */
#define IOCTL_ITERS        1000
#define THROUGHPUT_PASSES  3

/* cache mode names */
static const char *cache_names[] = { "WC", "WB", "UC", "WT" };

/* --- stats engine --- */
struct bench_stats {
	unsigned long *samples;
	int count;
	int capacity;
};

static int bench_stats_init(struct bench_stats *s, int cap)
{
	s->samples = calloc(cap, sizeof(unsigned long));
	if (!s->samples)
		return -1;
	s->count = 0;
	s->capacity = cap;
	return 0;
}

static void bench_stats_add(struct bench_stats *s, unsigned long val)
{
	if (s->count < s->capacity)
		s->samples[s->count++] = val;
}

static int cmp_ulong(const void *a, const void *b)
{
	unsigned long va = *(const unsigned long *)a;
	unsigned long vb = *(const unsigned long *)b;
	if (va < vb) return -1;
	if (va > vb) return 1;
	return 0;
}

static void bench_stats_compute(struct bench_stats *s,
				unsigned long *min_out,
				unsigned long *max_out,
				double *avg_out,
				unsigned long *med_out)
{
	int i;
	double sum = 0;

	if (s->count == 0) {
		*min_out = *max_out = *med_out = 0;
		*avg_out = 0.0;
		return;
	}

	qsort(s->samples, s->count, sizeof(unsigned long), cmp_ulong);

	*min_out = s->samples[0];
	*max_out = s->samples[s->count - 1];
	*med_out = s->samples[s->count / 2];

	for (i = 0; i < s->count; i++)
		sum += (double)s->samples[i];
	*avg_out = sum / (double)s->count;
}

static void bench_stats_free(struct bench_stats *s)
{
	free(s->samples);
	s->samples = NULL;
	s->count = 0;
}

static void bench_stats_print(const char *label, struct bench_stats *s,
			      const char *unit)
{
	unsigned long min_v, max_v, med_v;
	double avg_v;

	bench_stats_compute(s, &min_v, &max_v, &avg_v, &med_v);
	printf("  %-30s min=%-8lu max=%-8lu avg=%-10.1f median=%-8lu %s\n",
	       label, min_v, max_v, avg_v, med_v, unit);
}

static inline unsigned long now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return (unsigned long)ts.tv_sec * 1000000000UL + (unsigned long)ts.tv_nsec;
}

/* --- page fault tracking via /proc/self/stat --- */
static long read_minflt(void)
{
	FILE *fp;
	char buf[512];
	long val = -1;
	int field = 0;
	char *tok;

	fp = fopen("/proc/self/stat", "r");
	if (!fp)
		return -1;
	if (!fgets(buf, sizeof(buf), fp)) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	tok = strtok(buf, " ");
	while (tok) {
		field++;
		if (field == 10) {
			val = atol(tok);
			break;
		}
		tok = strtok(NULL, " ");
	}
	return val;
}

/* --- debugfs helpers --- */
static int read_debugfs_status(const char *key, unsigned long *val)
{
	FILE *fp;
	char line[256];

	fp = fopen(STATS_PATH, "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, key)) {
			char *p = strchr(line, ':');
			if (p)
				*val = strtoul(p + 1, NULL, 0);
			fclose(fp);
			return 0;
		}
	}
	fclose(fp);
	return -1;
}

static int read_cache_policy_mode(int *mode_out)
{
	FILE *fp;
	char line[128];
	int mode = -1;

	fp = fopen(CACHE_POLICY_PATH, "r");
	if (!fp)
		return -1;

	/* skip header, read the "all" line */
	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "----") || strstr(line, "range"))
			continue;
		if (strstr(line, "all")) {
			char *tok = strtok(line, " \t\n");
			char *last = tok;
			while (tok) { last = tok; tok = strtok(NULL, " \t\n"); }
			if (last) {
				if (strcmp(last, "WC") == 0) mode = PTEMAP_CACHE_WC;
				else if (strcmp(last, "WB") == 0) mode = PTEMAP_CACHE_WB;
				else if (strcmp(last, "UC") == 0) mode = PTEMAP_CACHE_UC;
				else if (strcmp(last, "WT") == 0) mode = PTEMAP_CACHE_WT;
			}
			break;
		}
	}
	fclose(fp);
	if (mode >= 0) { *mode_out = mode; return 0; }
	return -1;
}

static int write_cache_policy_mode(int mode)
{
	FILE *fp;
	fp = fopen(CACHE_POLICY_PATH, "w");
	if (!fp)
		return -1;
	fprintf(fp, "all %s\n", cache_names[mode]);
	fclose(fp);
	return 0;
}

/* --- ioctl helpers --- */
static int detect_direct_pte(void)
{
	FILE *fp;
	char line[128];

	fp = fopen("/sys/kernel/debug/ptemap/status", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "direct_pte")) {
			fclose(fp);
			return (strstr(line, "1") != NULL) ? 1 : 0;
		}
	}
	fclose(fp);
	return -1;
}

static int detect_huge_page(void)
{
	FILE *fp;
	char line[128];

	fp = fopen("/sys/kernel/debug/ptemap/status", "r");
	if (!fp)
		return -1;

	while (fgets(line, sizeof(line), fp)) {
		if (strstr(line, "page_size") && strstr(line, "2MB")) {
			fclose(fp);
			return 2;
		}
	}
	fclose(fp);
	return 0;
}

/* --- mmap wrappers --- */
struct mmap_ctx {
	void *ptr;
	unsigned long size;
	int fd;
	unsigned long nr_pages;
	unsigned long page_size;
};

static int do_mmap_zerofault(struct mmap_ctx *ctx, unsigned long nr_pages)
{
	int fd;

	fd = open("/dev/ptemap", O_RDWR);
	if (fd < 0) {
		perror("open /dev/ptemap");
		return -1;
	}

	/* detect page size */
	{
		struct ptemap_query_req req;
		req.page_idx = 0;
		if (ioctl(fd, PTEMAP_IOC_QUERY, &req) < 0) {
			perror("ioctl QUERY for page_size");
			close(fd);
			return -1;
		}
		ctx->page_size = req.page_size;
	}

	ctx->nr_pages = nr_pages;
	ctx->size = nr_pages * ctx->page_size;
	ctx->fd = fd;
	ctx->ptr = mmap(NULL, ctx->size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	if (ctx->ptr == MAP_FAILED) {
		perror("mmap /dev/ptemap");
		close(fd);
		return -1;
	}
	return 0;
}

static void do_munmap_zerofault(struct mmap_ctx *ctx)
{
	if (ctx->ptr && ctx->ptr != MAP_FAILED)
		munmap(ctx->ptr, ctx->size);
	if (ctx->fd >= 0)
		close(ctx->fd);
	ctx->ptr = NULL;
	ctx->fd = -1;
}

static int do_mmap_anon(struct mmap_ctx *ctx, unsigned long nr_pages,
			int populate)
{
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (populate)
		flags |= MAP_POPULATE;

	ctx->page_size = 4096;
	ctx->nr_pages = nr_pages;
	ctx->size = nr_pages * ctx->page_size;
	ctx->fd = -1;
	ctx->ptr = mmap(NULL, ctx->size, PROT_READ | PROT_WRITE,
			flags, -1, 0);
	if (ctx->ptr == MAP_FAILED) {
		perror("mmap MAP_ANONYMOUS");
		return -1;
	}
	return 0;
}

static void do_munmap_anon(struct mmap_ctx *ctx)
{
	if (ctx->ptr && ctx->ptr != MAP_FAILED)
		munmap(ctx->ptr, ctx->size);
	ctx->ptr = NULL;
}

/* Wrappers to match the bench function pointer signature */
static int do_mmap_anon_wrap(struct mmap_ctx *ctx, unsigned long nr_pages)
{
	return do_mmap_anon(ctx, nr_pages, 0);
}

static int do_mmap_populate_wrap(struct mmap_ctx *ctx, unsigned long nr_pages)
{
	return do_mmap_anon(ctx, nr_pages, 1);
}

/* --- benchmark: mmap latency --- */
static void bench_mmap_latency(const char *mode_name,
			       int (*do_mmap)(struct mmap_ctx *, unsigned long),
			       void (*do_munmap)(struct mmap_ctx *),
			       unsigned long nr_pages, int iterations)
{
	struct bench_stats s;
	struct mmap_ctx ctx;
	int i;
	unsigned long t0, t1;
	long pf1, pf2;

	bench_stats_init(&s, iterations);

	/* warmup */
	memset(&ctx, 0, sizeof(ctx));
	if (do_mmap(&ctx, nr_pages) == 0)
		do_munmap(&ctx);

	pf1 = read_minflt();

	for (i = 0; i < iterations; i++) {
		memset(&ctx, 0, sizeof(ctx));
		t0 = now_ns();
		if (do_mmap(&ctx, nr_pages) != 0)
			break;
		t1 = now_ns();
		bench_stats_add(&s, t1 - t0);
		do_munmap(&ctx);
	}

	pf2 = read_minflt();

	printf("\n--- mmap latency (%s, %lu pages, %d iterations) ---\n",
	       mode_name, nr_pages, s.count);
	if (pf1 >= 0 && pf2 >= 0)
		printf("  page_faults: pre=%ld post=%ld delta=%ld\n",
		       pf1, pf2, pf2 - pf1);
	bench_stats_print("mmap (ns)", &s, "");
	bench_stats_free(&s);
}

/* --- benchmark: first-access latency --- */
static void bench_first_access(const char *mode_name,
			       int (*do_mmap)(struct mmap_ctx *, unsigned long),
			       void (*do_munmap)(struct mmap_ctx *),
			       unsigned long nr_pages)
{
	struct mmap_ctx ctx;
	unsigned long i, t0, t1;
	unsigned long min_v = ~0UL, max_v = 0, sum = 0, med_v;
	unsigned long outlier_1us = 0;
	volatile unsigned long dummy;
	unsigned char *base;
	long pf1, pf2;

	memset(&ctx, 0, sizeof(ctx));
	if (do_mmap(&ctx, nr_pages) != 0)
		return;

	pf1 = read_minflt();
	base = (unsigned char *)ctx.ptr;

	for (i = 0; i < ctx.nr_pages; i++) {
		volatile unsigned long *p =
			(volatile unsigned long *)(base + i * ctx.page_size);
		t0 = now_ns();
		dummy = *p;
		/* compiler barrier to prevent reordering */
		__asm__ __volatile__("" : : "r"(dummy) : "memory");
		t1 = now_ns();

		unsigned long dt = t1 - t0;
		if (dt < min_v) min_v = dt;
		if (dt > max_v) max_v = dt;
		sum += dt;
		if (dt > 1000)
			outlier_1us++;
	}
	pf2 = read_minflt();

	med_v = sum / ctx.nr_pages;

	printf("\n--- first-access latency (%s, %lu pages) ---\n",
	       mode_name, ctx.nr_pages);
	if (pf1 >= 0 && pf2 >= 0)
		printf("  page_faults: pre=%ld post=%ld delta=%ld\n",
		       pf1, pf2, pf2 - pf1);
	printf("  %-30s min=%-8lu max=%-8lu avg=%-10.1f median~%-8lu ns\n",
	       "first-read (per page)", min_v, max_v, (double)sum / ctx.nr_pages, med_v);
	printf("  >1us outliers: %lu / %lu pages (%.1f%%)\n",
	       outlier_1us, ctx.nr_pages,
	       100.0 * (double)outlier_1us / (double)ctx.nr_pages);

	do_munmap(&ctx);
}

/* --- benchmark: read throughput --- */
static void bench_read_throughput(const char *mode_name,
				  int (*do_mmap)(struct mmap_ctx *, unsigned long),
				  void (*do_munmap)(struct mmap_ctx *),
				  unsigned long nr_pages)
{
	struct mmap_ctx ctx;
	struct bench_stats s;
	int pass;
	volatile unsigned long sum = 0;  /* volatile to prevent dead-code elim */
	volatile unsigned long *p;
	unsigned long nwords;
	unsigned long i, t0, t1;
	double MB;

	memset(&ctx, 0, sizeof(ctx));
	if (do_mmap(&ctx, nr_pages) != 0)
		return;

	nwords = ctx.size / sizeof(unsigned long);
	p = (volatile unsigned long *)ctx.ptr;
	MB = (double)ctx.size / (1024.0 * 1024.0);

	/* warmup */
	for (i = 0; i < nwords; i++)
		sum += p[i];

	bench_stats_init(&s, THROUGHPUT_PASSES);

	for (pass = 0; pass < THROUGHPUT_PASSES; pass++) {
		t0 = now_ns();
		for (i = 0; i < nwords; i++)
			sum += p[i];
		t1 = now_ns();

		double sec = (double)(t1 - t0) / 1e9;
		double mbps = MB / sec;
		bench_stats_add(&s, (unsigned long)mbps);
	}

	printf("\n--- read throughput (%s, %.2f MB, %d passes) ---\n",
	       mode_name, MB, THROUGHPUT_PASSES);
	printf("  (volatile accumulator=0x%lx prevents dead-code elimination)\n",
	       (unsigned long)(sum & 0xffff));
	bench_stats_print("read (MB/s)", &s, "");

	bench_stats_free(&s);
	do_munmap(&ctx);
}

/* --- benchmark: write throughput --- */
static void bench_write_throughput(const char *mode_name,
				   int (*do_mmap)(struct mmap_ctx *, unsigned long),
				   void (*do_munmap)(struct mmap_ctx *),
				   unsigned long nr_pages)
{
	struct mmap_ctx ctx;
	struct bench_stats s;
	int pass;
	volatile unsigned long *p;
	unsigned long nwords;
	unsigned long i, t0, t1;
	double MB;

	memset(&ctx, 0, sizeof(ctx));
	if (do_mmap(&ctx, nr_pages) != 0)
		return;

	nwords = ctx.size / sizeof(unsigned long);
	p = (volatile unsigned long *)ctx.ptr;
	MB = (double)ctx.size / (1024.0 * 1024.0);

	/* warmup */
	for (i = 0; i < nwords; i++)
		p[i] = i;

	bench_stats_init(&s, THROUGHPUT_PASSES);

	for (pass = 0; pass < THROUGHPUT_PASSES; pass++) {
		unsigned long val = pass;
		t0 = now_ns();
		for (i = 0; i < nwords; i++)
			p[i] = val + i;
		t1 = now_ns();

		double sec = (double)(t1 - t0) / 1e9;
		double mbps = MB / sec;
		bench_stats_add(&s, (unsigned long)mbps);
	}

	printf("\n--- write throughput (%s, %.2f MB, %d passes) ---\n",
	       mode_name, MB, THROUGHPUT_PASSES);
	bench_stats_print("write (MB/s)", &s, "");

	bench_stats_free(&s);
	do_munmap(&ctx);
}

/* --- benchmark: ioctl latency (zerofault only) --- */
static void bench_ioctl_latency(unsigned long nr_pages)
{
	struct mmap_ctx ctx;
	struct bench_stats s;
	int i;

	if (do_mmap_zerofault(&ctx, nr_pages) != 0)
		return;

	/* QUERY single page */
	bench_stats_init(&s, IOCTL_ITERS);
	for (i = 0; i < IOCTL_ITERS; i++) {
		struct ptemap_query_req req;
		unsigned long t0, t1;

		req.page_idx = i % nr_pages;
		t0 = now_ns();
		ioctl(ctx.fd, PTEMAP_IOC_QUERY, &req);
		t1 = now_ns();
		bench_stats_add(&s, t1 - t0);
	}
	printf("\n--- ioctl latency (%lu iterations each) ---\n",
	       (unsigned long)IOCTL_ITERS);
	bench_stats_print("QUERY (single page, ns)", &s, "");
	bench_stats_free(&s);

	/* QUERY_RANGE batch */
	{
		__u32 *cache_buf = calloc(nr_pages, sizeof(__u32));
		__u64 *pfn_buf = calloc(nr_pages, sizeof(__u64));
		__u64 *va_buf = calloc(nr_pages, sizeof(__u64));
		struct ptemap_query_range_req rreq;

		rreq.start_idx = 0;
		rreq.nr_pages = nr_pages;
		rreq.pfn_buf = pfn_buf;
		rreq.vaddr_buf = va_buf;
		rreq.cache_buf = cache_buf;

		bench_stats_init(&s, IOCTL_ITERS);
		for (i = 0; i < IOCTL_ITERS; i++) {
			unsigned long t0, t1;
			t0 = now_ns();
			ioctl(ctx.fd, PTEMAP_IOC_QUERY_RANGE, &rreq);
			t1 = now_ns();
			bench_stats_add(&s, t1 - t0);
		}
		bench_stats_print("QUERY_RANGE (batch, ns)", &s, "");
		bench_stats_free(&s);

		free(cache_buf);
		free(pfn_buf);
		free(va_buf);
	}

	/* FLUSH_TLB */
	bench_stats_init(&s, IOCTL_ITERS);
	for (i = 0; i < IOCTL_ITERS; i++) {
		unsigned long t0, t1;
		t0 = now_ns();
		ioctl(ctx.fd, PTEMAP_IOC_FLUSH_TLB);
		t1 = now_ns();
		bench_stats_add(&s, t1 - t0);
	}
	bench_stats_print("FLUSH_TLB (ns)", &s, "");
	bench_stats_free(&s);

	do_munmap_zerofault(&ctx);
}

/* --- benchmark: cache mode impact (zerofault only) --- */
static void bench_cache_mode_impact(unsigned long nr_pages)
{
	int orig_mode, mode;
	struct bench_stats s_read[PTEMAP_CACHE_NR];
	struct bench_stats s_write[PTEMAP_CACHE_NR];
	int valid[PTEMAP_CACHE_NR] = {0};

	if (read_cache_policy_mode(&orig_mode) != 0) {
		printf("\n--- cache mode impact: cannot read cache_policy, skipping ---\n");
		return;
	}

	printf("\n--- cache mode comparison (read/write throughput, %lu pages) ---\n",
	       nr_pages);
	printf("  %-6s %-16s %-16s\n", "mode", "read(MB/s)", "write(MB/s)");
	printf("  %-6s %-16s %-16s\n", "------", "----------------", "----------------");

	for (mode = 0; mode < PTEMAP_CACHE_NR; mode++) {
		struct mmap_ctx ctx;
		volatile unsigned long *p, sum = 0;
		double MB;
		unsigned long nwords, i;
		int pass;

		if (write_cache_policy_mode(mode) != 0)
			continue;

		memset(&ctx, 0, sizeof(ctx));
		if (do_mmap_zerofault(&ctx, nr_pages) != 0)
			continue;

		nwords = ctx.size / sizeof(unsigned long);
		p = (volatile unsigned long *)ctx.ptr;
		MB = (double)ctx.size / (1024.0 * 1024.0);

		/* read throughput */
		bench_stats_init(&s_read[mode], THROUGHPUT_PASSES);
		/* warmup */
		for (i = 0; i < nwords; i++) sum += p[i];
		for (pass = 0; pass < THROUGHPUT_PASSES; pass++) {
			unsigned long t0 = now_ns();
			for (i = 0; i < nwords; i++) sum += p[i];
			unsigned long t1 = now_ns();
			double sec = (double)(t1 - t0) / 1e9;
			bench_stats_add(&s_read[mode], (unsigned long)(MB / sec));
		}

		/* write throughput */
		bench_stats_init(&s_write[mode], THROUGHPUT_PASSES);
		/* warmup */
		for (i = 0; i < nwords; i++) p[i] = i;
		for (pass = 0; pass < THROUGHPUT_PASSES; pass++) {
			unsigned long val = pass;
			unsigned long t0 = now_ns();
			for (i = 0; i < nwords; i++) p[i] = val + i;
			unsigned long t1 = now_ns();
			double sec = (double)(t1 - t0) / 1e9;
			bench_stats_add(&s_write[mode], (unsigned long)(MB / sec));
		}

		valid[mode] = 1;
		do_munmap_zerofault(&ctx);

		/* print row */
		{
			unsigned long r_min, r_max, r_med, w_min, w_max, w_med;
			double r_avg, w_avg;
			bench_stats_compute(&s_read[mode], &r_min, &r_max, &r_avg, &r_med);
			bench_stats_compute(&s_write[mode], &w_min, &w_max, &w_avg, &w_med);
			printf("  %-6s %-8.0f (%5lu)    %-8.0f (%5lu)\n",
			       cache_names[mode], r_avg, r_med, w_avg, w_med);
		}
	}

	/* restore original cache mode */
	write_cache_policy_mode(orig_mode);

	for (mode = 0; mode < PTEMAP_CACHE_NR; mode++) {
		if (valid[mode]) {
			bench_stats_free(&s_read[mode]);
			bench_stats_free(&s_write[mode]);
		}
	}
}

/* --- kernel debugfs stats --- */
static void bench_print_kernel_stats(void)
{
	FILE *fp;
	char line[256];

	fp = fopen(STATS_PATH, "r");
	if (!fp)
		return;

	printf("\n--- kernel debugfs stats ---\n");
	while (fgets(line, sizeof(line), fp)) {
		fputs(line, stdout);
	}
	fclose(fp);
}

/* --- mode: zerofault --- */
static void run_zerofault(unsigned long nr_pages, int iterations)
{
	int direct_pte = detect_direct_pte();
	int huge = detect_huge_page();
	char mode_name[64];

	if (huge == 2)
		snprintf(mode_name, sizeof(mode_name), "zerofault (2MB huge)");
	else if (direct_pte == 1)
		snprintf(mode_name, sizeof(mode_name), "zerofault (direct PTE)");
	else
		snprintf(mode_name, sizeof(mode_name), "zerofault (vm_insert_page)");

	bench_mmap_latency(mode_name, do_mmap_zerofault, do_munmap_zerofault,
			   nr_pages, iterations);
	bench_first_access(mode_name, do_mmap_zerofault, do_munmap_zerofault,
			   nr_pages);
	bench_read_throughput(mode_name, do_mmap_zerofault, do_munmap_zerofault,
			      nr_pages);
	bench_write_throughput(mode_name, do_mmap_zerofault, do_munmap_zerofault,
			       nr_pages);
	bench_ioctl_latency(nr_pages);
	bench_cache_mode_impact(nr_pages);
	bench_print_kernel_stats();
}

/* --- mode: anon --- */
static void run_anon(unsigned long nr_pages, int iterations, int populate)
{
	const char *mode_name = populate ? "MAP_POPULATE" : "MAP_ANONYMOUS";
	int (*mmap_fn)(struct mmap_ctx *, unsigned long) = populate
		? do_mmap_populate_wrap : do_mmap_anon_wrap;

	bench_mmap_latency(mode_name, mmap_fn, do_munmap_anon,
			   nr_pages, iterations);
	bench_first_access(mode_name, mmap_fn, do_munmap_anon,
			   nr_pages);
	bench_read_throughput(mode_name, mmap_fn, do_munmap_anon,
			      nr_pages);
	bench_write_throughput(mode_name, mmap_fn, do_munmap_anon,
			       nr_pages);
}

/* --- mode: compare --- */
static void run_compare(unsigned long nr_pages, int iterations)
{
	int has_ptemap = (open("/dev/ptemap", O_RDWR) >= 0);

	printf("\n=== comparison: mmap + first-access (%lu pages) ===\n\n", nr_pages);
	printf("  %-25s %-14s %-16s %s\n",
	       "strategy", "mmap(ns)", "first-touch(ns)", "page-faults");
	printf("  %-25s %-14s %-16s %s\n",
	       "-------------------------", "--------------",
	       "----------------", "-----------");

	if (has_ptemap) {
		close(open("/dev/ptemap", O_RDWR)); /* just probe, close */
		/* Zerofault with current module config */
		{
			struct bench_stats s_mmap;
			struct mmap_ctx ctx;
			long pf1, pf2, fa_ns = 0;
			int i;

			bench_stats_init(&s_mmap, iterations);
			for (i = 0; i < iterations; i++) {
				memset(&ctx, 0, sizeof(ctx));
				unsigned long t0 = now_ns();
				if (do_mmap_zerofault(&ctx, nr_pages) != 0)
					break;
				unsigned long t1 = now_ns();
				bench_stats_add(&s_mmap, t1 - t0);
				do_munmap_zerofault(&ctx);
			}

			/* first-access (once) */
			memset(&ctx, 0, sizeof(ctx));
			if (do_mmap_zerofault(&ctx, nr_pages) == 0) {
				volatile unsigned char *base = (volatile unsigned char *)ctx.ptr;
				unsigned long j;
				pf1 = read_minflt();
				unsigned long t0 = now_ns();
				for (j = 0; j < ctx.nr_pages; j++)
					*(volatile unsigned long *)(base + j * ctx.page_size);
				unsigned long t1 = now_ns();
				pf2 = read_minflt();
				fa_ns = t1 - t0;
				do_munmap_zerofault(&ctx);

				unsigned long m_min, m_max, m_med;
				double m_avg;
				bench_stats_compute(&s_mmap, &m_min, &m_max, &m_avg, &m_med);
				printf("  %-25s %-8.0f (%5lu)  %-8lu (%5lu)  %ld\n",
				       "zerofault", m_avg, m_med,
				       fa_ns, fa_ns, pf2 - pf1);
			}
			bench_stats_free(&s_mmap);
		}
	}

	/* MAP_ANONYMOUS */
	{
		struct bench_stats s_mmap;
		struct mmap_ctx ctx;
		long pf1, pf2, fa_ns = 0;
		int i;

		bench_stats_init(&s_mmap, iterations);
		for (i = 0; i < iterations; i++) {
			memset(&ctx, 0, sizeof(ctx));
			unsigned long t0 = now_ns();
			if (do_mmap_anon_wrap(&ctx, nr_pages) != 0)
				break;
			unsigned long t1 = now_ns();
			bench_stats_add(&s_mmap, t1 - t0);
			do_munmap_anon(&ctx);
		}

		memset(&ctx, 0, sizeof(ctx));
		if (do_mmap_anon_wrap(&ctx, nr_pages) == 0) {
			volatile unsigned char *base = (volatile unsigned char *)ctx.ptr;
			unsigned long j;
			pf1 = read_minflt();
			unsigned long t0 = now_ns();
			for (j = 0; j < ctx.nr_pages; j++)
				*(volatile unsigned long *)(base + j * ctx.page_size);
			unsigned long t1 = now_ns();
			pf2 = read_minflt();
			fa_ns = t1 - t0;
			do_munmap_anon(&ctx);

			unsigned long m_min, m_max, m_med;
			double m_avg;
			bench_stats_compute(&s_mmap, &m_min, &m_max, &m_avg, &m_med);
			printf("  %-25s %-8.0f (%5lu)  %-8lu (%5lu)  %ld\n",
			       "MAP_ANONYMOUS", m_avg, m_med,
			       fa_ns, fa_ns, pf2 - pf1);
		}
		bench_stats_free(&s_mmap);
	}

	/* MAP_POPULATE */
	{
		struct bench_stats s_mmap;
		struct mmap_ctx ctx;
		long pf1, pf2, fa_ns = 0;
		int i;

		bench_stats_init(&s_mmap, iterations);
		for (i = 0; i < iterations; i++) {
			memset(&ctx, 0, sizeof(ctx));
			unsigned long t0 = now_ns();
			if (do_mmap_populate_wrap(&ctx, nr_pages) != 0)
				break;
			unsigned long t1 = now_ns();
			bench_stats_add(&s_mmap, t1 - t0);
			do_munmap_anon(&ctx);
		}

		memset(&ctx, 0, sizeof(ctx));
		if (do_mmap_populate_wrap(&ctx, nr_pages) == 0) {
			volatile unsigned char *base = (volatile unsigned char *)ctx.ptr;
			unsigned long j;
			pf1 = read_minflt();
			unsigned long t0 = now_ns();
			for (j = 0; j < ctx.nr_pages; j++)
				*(volatile unsigned long *)(base + j * ctx.page_size);
			unsigned long t1 = now_ns();
			pf2 = read_minflt();
			fa_ns = t1 - t0;
			do_munmap_anon(&ctx);

			unsigned long m_min, m_max, m_med;
			double m_avg;
			bench_stats_compute(&s_mmap, &m_min, &m_max, &m_avg, &m_med);
			printf("  %-25s %-8.0f (%5lu)  %-8lu (%5lu)  %ld\n",
			       "MAP_POPULATE", m_avg, m_med,
			       fa_ns, fa_ns, pf2 - pf1);
		}
		bench_stats_free(&s_mmap);
	}

	has_ptemap = has_ptemap;  /* suppress unused warning */
}

/* --- main --- */
static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <mode> [nr_pages=%d] [iterations=%d]\n"
		"\n"
		"Modes:\n"
		"  zerofault   Benchmark /dev/ptemap (all sub-benchmarks)\n"
		"  anon        Benchmark MAP_ANONYMOUS | MAP_PRIVATE\n"
		"  populate    Benchmark MAP_ANONYMOUS | MAP_PRIVATE | MAP_POPULATE\n"
		"  compare     Compare all available strategies (summary table)\n"
		"\n"
		"Examples:\n"
		"  insmod ptemap.ko phys_pages=256 use_direct_pte=1\n"
		"  %s zerofault 256\n"
		"  rmmod ptemap && %s anon 256\n"
		"  %s compare 256\n",
		prog, DEFAULT_NR_PAGES, DEFAULT_ITER,
		prog, prog, prog);
}

int main(int argc, char *argv[])
{
	const char *mode;
	unsigned long nr_pages = DEFAULT_NR_PAGES;
	int iterations = DEFAULT_ITER;

	if (argc < 2) {
		usage(argv[0]);
		return 1;
	}

	mode = argv[1];
	if (argc >= 3)
		nr_pages = strtoul(argv[2], NULL, 0);
	if (argc >= 4)
		iterations = atoi(argv[3]);

	if (iterations < 1)
		iterations = DEFAULT_ITER;

	printf("=== bench_ptemap v1.0 ======================================\n");
	printf("mode: %s | nr_pages: %lu | iterations: %d\n",
	       mode, nr_pages, iterations);
	printf("NOTE: QEMU VM timings — relative comparison only, not bare-metal.\n");

	if (strcmp(mode, "zerofault") == 0) {
		run_zerofault(nr_pages, iterations);
	} else if (strcmp(mode, "anon") == 0) {
		run_anon(nr_pages, iterations, 0);
	} else if (strcmp(mode, "populate") == 0) {
		run_anon(nr_pages, iterations, 1);
	} else if (strcmp(mode, "compare") == 0) {
		run_compare(nr_pages, iterations);
	} else {
		fprintf(stderr, "Unknown mode: %s\n", mode);
		usage(argv[0]);
		return 1;
	}

	printf("\n=== bench_ptemap: done ===\n");
	return 0;
}

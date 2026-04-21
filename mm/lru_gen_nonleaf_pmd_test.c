// SPDX-License-Identifier: GPL-2.0
/*
 * Test module for LRU Gen non-leaf PMD young bit support.
 *
 * This module provides debugfs interfaces to test and measure the impact of
 * the non-leaf PMD young bit feature on page table walking performance,
 * which is used by the multi-generation LRU (MGLRU) page reclaim code to
 * skip scanning PTEs under non-young PMD entries.
 *
 * Usage:
 *   modprobe lru_gen_nonleaf_pmd_test
 *   or build as module: CONFIG_LRU_GEN_NONLEAF_PMD_TEST=m
 *
 *   # Check if the platform supports non-leaf PMD young
 *   cat /sys/kernel/debug/lru_gen_nonleaf_pmd/capability
 *
 *   # Run the page table walk benchmark (size in MB)
 *   echo 256 > /sys/kernel/debug/lru_gen_nonleaf_pmd/run_walk_test
 *   cat /sys/kernel/debug/lru_gen_nonleaf_pmd/walk_test_result
 *
 *   # Run PMD young clear test (size in MB)
 *   echo 512 > /sys/kernel/debug/lru_gen_nonleaf_pmd/run_pmd_clear_test
 *   cat /sys/kernel/debug/lru_gen_nonleaf_pmd/pmd_clear_result
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/module.h>
#include <linux/pgtable.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <asm/tlbflush.h>

#define DEFAULT_TEST_SIZE_MB	256

/*
 * Number of warmup iterations to stabilize page cache and TLB.
 * Each iteration allocates, touches, and frees memory to ensure
 * consistent state for the actual test.
 */
#define WARMUP_ITERATIONS	2

/*
 * Delay between test iterations (in milliseconds) to allow
 * background page reclaim and TLB flush to complete.
 */
#define TEST_DELAY_MS		100

static struct dentry *debugfs_root;

/*
 * Page table walk test result.
 */
struct walk_test_result {
	ktime_t total_time_ns;
	unsigned long nr_pmds;
	unsigned long nr_pmds_young;
	unsigned long nr_pmds_old;
	unsigned long nr_ptes;
	unsigned long nr_ptes_young;
};

static struct walk_test_result walk_result;

/*
 * Extended test result for multiple iterations.
 */
struct walk_test_stats {
	unsigned long iterations;
	unsigned long completed;
	ktime_t min_time_ns;
	ktime_t max_time_ns;
	ktime_t avg_time_ns;
	ktime_t total_time_ns;
	unsigned long total_pmds;
	unsigned long total_ptes;
};

static struct walk_test_stats walk_stats;

/*
 * PMD clear test result.
 */
struct pmd_clear_result {
	ktime_t total_time_ns;
	unsigned long nr_cleared;
	unsigned long nr_not_young;
	unsigned long nr_skipped;
};

static struct pmd_clear_result pmd_clear_result;

static bool has_nonleaf_pmd_young(void)
{
	return IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) &&
	       arch_has_hw_nonleaf_pmd_young();
}

/*
 * Strictly initialize all pages in the given range.
 *
 * This function ensures that:
 * 1. All pages are physically allocated (trigger page faults)
 * 2. All PTEs are established with young bit set
 * 3. Both read and write access patterns are exercised
 * 4. Memory barriers ensure visibility of all operations
 *
 * Returns the number of pages successfully initialized.
 */
static unsigned long init_all_pages(unsigned long addr, unsigned long size)
{
	unsigned char __user *p = (unsigned char __user *)addr;
	unsigned long i;

	/*
	 * First pass: write to each page to trigger page faults
	 * and establish writable PTE mappings with young bit set.
	 */
	for (i = 0; i < size; i += PAGE_SIZE) {
		if (put_user(0xAB, &p[i]))
			break;
	}

	/*
	 * Second pass: read from each page to ensure
	 * young bit is firmly set and TLB entries are populated.
	 */
	if (i == size) {
		unsigned char val;
		for (i = 0; i < size; i += PAGE_SIZE) {
			if (get_user(val, &p[i]))
				break;
		}
	}

	/* Memory barrier to ensure all operations are visible */
	mb();

	return i / PAGE_SIZE;
}

/*
 * Perform warmup iterations to stabilize the system state.
 *
 * This allocates, initializes, and frees memory multiple times
 * to ensure that:
 * - Page cache is primed
 * - Memory allocator is in steady state
 * - TLB entries are populated
 * - Background reclaim has settled
 */
static void warmup_test(size_t size_mb, int iterations)
{
	unsigned long addr, size;
	unsigned long nr_pages;
	int i;

	size = size_mb * 1024 * 1024;

	for (i = 0; i < iterations; i++) {
		addr = vm_mmap(NULL, 0, size, PROT_READ | PROT_WRITE,
			       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 0);
		if (IS_ERR_VALUE(addr))
			continue;

		/* Initialize all pages strictly */
		nr_pages = init_all_pages(addr, size);

		pr_debug("lru_gen_nonleaf_pmd: warmup iter %d: %lu pages initialized\n",
			 i, nr_pages);

		/* Unmap to free the pages */
		vm_munmap(addr, size);

		/* Small delay between iterations */
		msleep(TEST_DELAY_MS);
	}
}

/*
 * Walk page tables and measure performance.
 *
 * This test allocates memory, strictly initializes all pages to establish
 * mappings with young bits set, performs warmup iterations to stabilize
 * the system state, then walks the page tables measuring how long it
 * takes to scan PMD and PTE entries and check their young bits.
 */
static int run_walk_test(size_t size_mb)
{
	struct mm_struct *mm = current->mm;
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	unsigned long addr, end, size;
	unsigned long nr_pmds = 0;
	unsigned long nr_pmds_young = 0;
	unsigned long nr_pmds_old = 0;
	unsigned long nr_ptes = 0;
	unsigned long nr_ptes_young = 0;
	unsigned long nr_pages_init;
	ktime_t start, elapsed;

	if (!mm)
		return -EINVAL;

	size = size_mb * 1024 * 1024;

	pr_info("lru_gen_nonleaf_pmd: walk test starting: %zu MB, warmup=%d iterations\n",
		size_mb, WARMUP_ITERATIONS);

	/*
	 * Perform warmup iterations to stabilize page cache and TLB state.
	 * This ensures consistent results across multiple test runs.
	 */
	warmup_test(size_mb, WARMUP_ITERATIONS);

	/* Allocate test memory */
	addr = vm_mmap(NULL, 0, size, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 0);
	if (IS_ERR_VALUE(addr))
		return -ENOMEM;

	end = addr + size;

	/*
	 * Strictly initialize ALL pages to ensure:
	 * 1. Physical pages are allocated
	 * 2. PTE mappings are established
	 * 3. Young bits are set on all entries
	 * 4. TLB entries are populated
	 */
	nr_pages_init = init_all_pages(addr, size);

	pr_info("lru_gen_nonleaf_pmd: initialized %lu pages out of %lu expected\n",
		nr_pages_init, size / PAGE_SIZE);

	if (nr_pages_init != size / PAGE_SIZE) {
		pr_warn("lru_gen_nonleaf_pmd: incomplete page initialization, aborting\n");
		vm_munmap(addr, size);
		return -EFAULT;
	}

	/* Walk page tables */
	mmap_read_lock(mm);
	start = ktime_get();

	vma_iter_init(&vmi, mm, addr);
	for_each_vma_range(vmi, vma, end) {
		unsigned long vma_end = min(vma->vm_end, end);
		pgd_t *pgd;
		p4d_t *p4d;
		pud_t *pud;
		pmd_t *pmd;

		for (unsigned long a = vma->vm_start; a < vma_end;
		     a = pgd_addr_end(a, vma_end)) {
			pgd_t pgd_val;
			p4d_t p4d_val;
			pud_t pud_val;
			pmd_t pmd_val;
			pte_t *pte;

			pgd = pgd_offset(mm, a);
			pgd_val = READ_ONCE(*pgd);
			if (pgd_none(pgd_val) || pgd_bad(pgd_val))
				continue;

			p4d = p4d_offset(pgd, a);
			p4d_val = READ_ONCE(*p4d);
			if (p4d_none(p4d_val) || p4d_bad(p4d_val))
				continue;

			pud = pud_offset(p4d, a);
			pud_val = READ_ONCE(*pud);
			if (pud_none(pud_val) || pud_bad(pud_val))
				continue;

			pmd = pmd_offset(pud, a);
			pmd_val = READ_ONCE(*pmd);
			if (pmd_none(pmd_val) || pmd_bad(pmd_val))
				continue;

			nr_pmds++;

			/* THP: check PMD young bit directly */
			if (pmd_trans_huge(pmd_val)) {
				if (pmd_young(pmd_val))
					nr_pmds_young++;
				else
					nr_pmds_old++;
				continue;
			}

			/* Non-leaf PMD: walk PTEs */
			pte = pte_offset_kernel(pmd, a);
			if (!pte)
				continue;

			for (unsigned long pa = a;
			     pa < min(pmd_addr_end(a, vma_end), vma_end);
			     pa += PAGE_SIZE, pte++) {
				pte_t pte_val = READ_ONCE(*pte);

				if (!pte_present(pte_val))
					continue;

				nr_ptes++;
				if (pte_young(pte_val))
					nr_ptes_young++;
			}
		}
	}

	elapsed = ktime_sub(ktime_get(), start);
	mmap_read_unlock(mm);

	/* Store results */
	walk_result.total_time_ns = elapsed;
	walk_result.nr_pmds = nr_pmds;
	walk_result.nr_pmds_young = nr_pmds_young;
	walk_result.nr_pmds_old = nr_pmds_old;
	walk_result.nr_ptes = nr_ptes;
	walk_result.nr_ptes_young = nr_ptes_young;

	pr_info("lru_gen_nonleaf_pmd: walk complete: %lu PMDs (%lu young, %lu old), %lu PTEs (%lu young), %lld ns\n",
		nr_pmds, nr_pmds_young, nr_pmds_old, nr_ptes, nr_ptes_young,
		ktime_to_ns(elapsed));

	vm_munmap(addr, size);
	return 0;
}

/*
 * Test pmdp_test_and_clear_young() performance.
 *
 * This measures the performance of clearing the young bit in PMD entries,
 * which is the exact code path used by LRU-Gen aging.
 *
 * Uses the same strict initialization and warmup as run_walk_test
 * to ensure consistent results across multiple runs.
 */
static int run_pmd_clear_test(size_t size_mb)
{
	struct mm_struct *mm = current->mm;
	struct vma_iterator vmi;
	struct vm_area_struct *vma;
	unsigned long addr, end, size;
	unsigned long nr_cleared = 0;
	unsigned long nr_not_young = 0;
	unsigned long nr_skipped = 0;
	unsigned long nr_pages_init;
	ktime_t start, elapsed;

	if (!mm)
		return -EINVAL;

	size = size_mb * 1024 * 1024;

	pr_info("lru_gen_nonleaf_pmd: pmd clear test starting: %zu MB, warmup=%d iterations\n",
		size_mb, WARMUP_ITERATIONS);

	/*
	 * Perform warmup iterations to stabilize page cache and TLB state.
	 * This ensures consistent results across multiple test runs.
	 */
	warmup_test(size_mb + 1, WARMUP_ITERATIONS);

	/* Allocate memory */
	addr = vm_mmap(NULL, 0, size + PMD_SIZE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, 0);
	if (IS_ERR_VALUE(addr))
		return -ENOMEM;

	/* Align to PMD boundary */
	addr = (addr + PMD_SIZE - 1) & PMD_MASK;
	end = addr + size;

	/*
	 * Strictly initialize ALL pages to ensure:
	 * 1. Physical pages are allocated
	 * 2. PTE mappings are established
	 * 3. Young bits are set on all entries
	 */
	nr_pages_init = init_all_pages(addr, size);

	pr_info("lru_gen_nonleaf_pmd: initialized %lu pages out of %lu expected\n",
		nr_pages_init, size / PAGE_SIZE);

	if (nr_pages_init != size / PAGE_SIZE) {
		pr_warn("lru_gen_nonleaf_pmd: incomplete page initialization, aborting\n");
		vm_munmap(addr, size);
		return -EFAULT;
	}

	mmap_read_lock(mm);
	start = ktime_get();

	vma_iter_init(&vmi, mm, addr);
	for_each_vma_range(vmi, vma, end) {
		unsigned long vma_end = min(vma->vm_end, end);

		for (unsigned long a = vma->vm_start & PMD_MASK;
		     a < vma_end; a += PMD_SIZE) {
			pmd_t *pmd;
			pmd_t pmd_val;

			pmd = pmd_off(mm, a);
			if (!pmd) {
				nr_skipped++;
				continue;
			}

			pmd_val = READ_ONCE(*pmd);
			if (!pmd_present(pmd_val) || !pmd_trans_huge(pmd_val)) {
				nr_skipped++;
				continue;
			}

			/* Clear the young bit */
			if (pmdp_test_and_clear_young(vma, a, pmd))
				nr_cleared++;
			else
				nr_not_young++;
		}
	}

	elapsed = ktime_sub(ktime_get(), start);
	mmap_read_unlock(mm);

	pmd_clear_result.total_time_ns = elapsed;
	pmd_clear_result.nr_cleared = nr_cleared;
	pmd_clear_result.nr_not_young = nr_not_young;
	pmd_clear_result.nr_skipped = nr_skipped;

	pr_info("lru_gen_nonleaf_pmd: clear test: %lu cleared, %lu not young, %lu skipped, %lld ns\n",
		nr_cleared, nr_not_young, nr_skipped, ktime_to_ns(elapsed));

	vm_munmap(addr, size);
	return 0;
}

/*
 * Debugfs interface: capability
 */
static int capability_show(struct seq_file *m, void *v)
{
	seq_printf(m, "has_nonleaf_pmd_young: %s\n",
		   has_nonleaf_pmd_young() ? "yes" : "no");
	seq_printf(m, "config_enabled: %s\n",
		   IS_ENABLED(CONFIG_ARCH_HAS_NONLEAF_PMD_YOUNG) ? "yes" : "no");

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(capability);

/*
 * Debugfs interface: walk test result
 */
static int walk_result_show(struct seq_file *m, void *v)
{
	seq_printf(m, "total_time_ns: %lld\n",
		   ktime_to_ns(walk_result.total_time_ns));
	seq_printf(m, "total_time_us: %lld\n",
		   ktime_to_us(walk_result.total_time_ns));
	seq_printf(m, "nr_pmds: %lu\n", walk_result.nr_pmds);
	seq_printf(m, "nr_pmds_young: %lu\n", walk_result.nr_pmds_young);
	seq_printf(m, "nr_pmds_old: %lu\n", walk_result.nr_pmds_old);
	seq_printf(m, "nr_ptes: %lu\n", walk_result.nr_ptes);
	seq_printf(m, "nr_ptes_young: %lu\n", walk_result.nr_ptes_young);

	if (walk_result.nr_pmds)
		seq_printf(m, "pmd_young_ratio: %lu%%\n",
			   walk_result.nr_pmds_young * 100 / walk_result.nr_pmds);
	if (walk_result.nr_ptes)
		seq_printf(m, "pte_young_ratio: %lu%%\n",
			   walk_result.nr_ptes_young * 100 / walk_result.nr_ptes);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(walk_result);

/*
 * Debugfs interface: PMD clear test result
 */
static int pmd_clear_result_show(struct seq_file *m, void *v)
{
	seq_printf(m, "total_time_ns: %lld\n",
		   ktime_to_ns(pmd_clear_result.total_time_ns));
	seq_printf(m, "total_time_us: %lld\n",
		   ktime_to_us(pmd_clear_result.total_time_ns));
	seq_printf(m, "nr_cleared: %lu\n", pmd_clear_result.nr_cleared);
	seq_printf(m, "nr_not_young: %lu\n", pmd_clear_result.nr_not_young);
	seq_printf(m, "nr_skipped: %lu\n", pmd_clear_result.nr_skipped);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(pmd_clear_result);

/*
 * Debugfs interface: run walk test
 */
static ssize_t walk_test_write(struct file *filp, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	char kbuf[32];
	size_t len = min_t(size_t, count, sizeof(kbuf) - 1);
	size_t size_mb = DEFAULT_TEST_SIZE_MB;

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	kbuf[len] = '\0';
	if (kstrtoul(kbuf, 10, &size_mb) == 0 && size_mb > 0)
		run_walk_test(size_mb);

	return count;
}

static const struct file_operations walk_test_fops = {
	.owner = THIS_MODULE,
	.write = walk_test_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/*
 * Debugfs interface: run walk test with multiple iterations
 * Format: "SIZE_MB,ITERATIONS" e.g., "1024,5"
 */
static ssize_t walk_test_iter_write(struct file *filp, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[64];
	size_t len = min_t(size_t, count, sizeof(kbuf) - 1);
	size_t size_mb = DEFAULT_TEST_SIZE_MB;
	unsigned long iterations = 5;
	char *comma;
	int i;

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	kbuf[len] = '\0';

	/* Parse "SIZE_MB,ITERATIONS" format */
	comma = strchr(kbuf, ',');
	if (comma) {
		*comma = '\0';
		if (kstrtoul(kbuf, 10, &size_mb) != 0 || size_mb == 0)
			return -EINVAL;
		if (kstrtoul(comma + 1, 10, &iterations) != 0 || iterations == 0)
			return -EINVAL;
	} else {
		if (kstrtoul(kbuf, 10, &size_mb) != 0 || size_mb == 0)
			return -EINVAL;
	}

	/* Initialize stats */
	walk_stats.iterations = iterations;
	walk_stats.completed = 0;
	walk_stats.min_time_ns = KTIME_MAX;
	walk_stats.max_time_ns = 0;
	walk_stats.total_time_ns = 0;
	walk_stats.total_pmds = 0;
	walk_stats.total_ptes = 0;

	pr_info("lru_gen_nonleaf_pmd: iterated walk test: %zu MB, %u iterations\n",
		size_mb, iterations);

	for (i = 0; i < iterations; i++) {
		ktime_t iter_time;

		/* Run single iteration */
		run_walk_test(size_mb);

		iter_time = walk_result.total_time_ns;

		/* Update stats */
		if (ktime_to_ns(iter_time) < ktime_to_ns(walk_stats.min_time_ns))
			walk_stats.min_time_ns = iter_time;
		if (ktime_to_ns(iter_time) > ktime_to_ns(walk_stats.max_time_ns))
			walk_stats.max_time_ns = iter_time;
		walk_stats.total_time_ns = ktime_add(walk_stats.total_time_ns, iter_time);
		walk_stats.total_pmds += walk_result.nr_pmds;
		walk_stats.total_ptes += walk_result.nr_ptes;
		walk_stats.completed++;

		pr_info("lru_gen_nonleaf_pmd: iteration %lu/%lu: %lld ns\n",
			i + 1, iterations, ktime_to_ns(iter_time));

		/* Delay between iterations */
		if (i < iterations - 1)
			msleep(TEST_DELAY_MS);
	}

	/* Calculate average */
	if (walk_stats.completed > 0)
		walk_stats.avg_time_ns = ns_to_ktime(ktime_to_ns(walk_stats.total_time_ns) /
						     walk_stats.completed);

	pr_info("lru_gen_nonleaf_pmd: iterated test complete: min=%lld ns, max=%lld ns, avg=%lld ns\n",
		ktime_to_ns(walk_stats.min_time_ns),
		ktime_to_ns(walk_stats.max_time_ns),
		ktime_to_ns(walk_stats.avg_time_ns));

	return count;
}

static const struct file_operations walk_test_iter_fops = {
	.owner = THIS_MODULE,
	.write = walk_test_iter_write,
	.open = simple_open,
	.llseek = default_llseek,
};

/*
 * Debugfs interface: walk test iteration statistics
 */
static int walk_stats_show(struct seq_file *m, void *v)
{
	if (walk_stats.completed == 0) {
		seq_puts(m, "No iterated test has been run yet.\n");
		return 0;
	}

	seq_printf(m, "iterations: %lu\n", walk_stats.iterations);
	seq_printf(m, "completed: %lu\n", walk_stats.completed);
	seq_printf(m, "min_time_ns: %lld\n", ktime_to_ns(walk_stats.min_time_ns));
	seq_printf(m, "max_time_ns: %lld\n", ktime_to_ns(walk_stats.max_time_ns));
	seq_printf(m, "avg_time_ns: %lld\n", ktime_to_ns(walk_stats.avg_time_ns));
	seq_printf(m, "total_time_ns: %lld\n", ktime_to_ns(walk_stats.total_time_ns));
	seq_printf(m, "total_pmds: %lu\n", walk_stats.total_pmds);
	seq_printf(m, "total_ptes: %lu\n", walk_stats.total_ptes);

	if (walk_stats.total_pmds)
		seq_printf(m, "avg_pmds_per_iter: %lu\n",
			   walk_stats.total_pmds / walk_stats.completed);
	if (walk_stats.total_ptes)
		seq_printf(m, "avg_ptes_per_iter: %lu\n",
			   walk_stats.total_ptes / walk_stats.completed);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(walk_stats);

/*
 * Debugfs interface: run PMD clear test
 */
static ssize_t pmd_clear_test_write(struct file *filp, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	char kbuf[32];
	size_t len = min_t(size_t, count, sizeof(kbuf) - 1);
	size_t size_mb = DEFAULT_TEST_SIZE_MB;

	if (copy_from_user(kbuf, buf, len))
		return -EFAULT;

	kbuf[len] = '\0';
	if (kstrtoul(kbuf, 10, &size_mb) == 0 && size_mb > 0)
		run_pmd_clear_test(size_mb);

	return count;
}

static const struct file_operations pmd_clear_test_fops = {
	.owner = THIS_MODULE,
	.write = pmd_clear_test_write,
	.open = simple_open,
	.llseek = default_llseek,
};

static int __init lru_gen_nonleaf_pmd_test_init(void)
{
	debugfs_root = debugfs_create_dir("lru_gen_nonleaf_pmd", NULL);

	debugfs_create_file("capability", 0444, debugfs_root, NULL,
			    &capability_fops);
	debugfs_create_file("walk_test_result", 0444, debugfs_root, NULL,
			    &walk_result_fops);
	debugfs_create_file("pmd_clear_result", 0444, debugfs_root, NULL,
			    &pmd_clear_result_fops);
	debugfs_create_file("run_walk_test", 0222, debugfs_root, NULL,
			    &walk_test_fops);
	debugfs_create_file("run_walk_test_iter", 0222, debugfs_root, NULL,
			    &walk_test_iter_fops);
	debugfs_create_file("walk_test_stats", 0444, debugfs_root, NULL,
			    &walk_stats_fops);
	debugfs_create_file("run_pmd_clear_test", 0222, debugfs_root, NULL,
			    &pmd_clear_test_fops);

	pr_info("lru_gen_nonleaf_pmd: loaded, non-leaf PMD young: %s\n",
		has_nonleaf_pmd_young() ? "YES" : "NO");

	return 0;
}

static void __exit lru_gen_nonleaf_pmd_test_exit(void)
{
	debugfs_remove_recursive(debugfs_root);
	pr_info("lru_gen_nonleaf_pmd: unloaded\n");
}

module_init(lru_gen_nonleaf_pmd_test_init);
module_exit(lru_gen_nonleaf_pmd_test_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Test module for LRU Gen non-leaf PMD young bit support");

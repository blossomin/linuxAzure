// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD SVM-SEV Host Support.
 *
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 *
 * Author: Ashish Kalra <ashish.kalra@amd.com>
 *
 */

#include <linux/cc_platform.h>
#include <linux/printk.h>
#include <linux/mm_types.h>
#include <linux/set_memory.h>
#include <linux/memblock.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/cpumask.h>
#include <linux/iommu.h>
#include <linux/amd-iommu.h>

#include <asm/sev.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/svm.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/apic.h>
#include <asm/cpuid.h>
#include <asm/cmdline.h>
#include <asm/iommu.h>

/*
 * The RMP entry format is not architectural. The format is defined in PPR
 * Family 19h Model 01h, Rev B1 processor.
 */
struct rmpentry {
	union {
		struct {
			u64 assigned	: 1,
			    pagesize	: 1,
			    immutable	: 1,
			    rsvd1	: 9,
			    gpa		: 39,
			    asid	: 10,
			    vmsa	: 1,
			    validated	: 1,
			    rsvd2	: 1;
		};
		u64 lo;
	};
	u64 hi;
} __packed;

/* Mask to apply to a PFN to get the first PFN of a 2MB page */
#define PFN_PMD_MASK	GENMASK_ULL(63, PMD_SHIFT - PAGE_SHIFT)

static u64 probed_rmp_base, probed_rmp_size;
static struct rmpentry *rmptable __ro_after_init;
static u64 rmptable_max_pfn __ro_after_init;

static LIST_HEAD(snp_leaked_pages_list);
static DEFINE_SPINLOCK(snp_leaked_pages_list_lock);

static unsigned long snp_nr_leaked_pages;

/* For synchronizing TCB updates with extended guest requests */
static DEFINE_MUTEX(snp_transaction_lock);
static u64 snp_transaction_id;
static bool snp_transaction_pending;

#undef pr_fmt
#define pr_fmt(fmt)	"SEV-SNP: " fmt

static int __mfd_enable(unsigned int cpu)
{
	u64 val;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	rdmsrl(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_MFDM;

	wrmsrl(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void mfd_enable(void *arg)
{
	__mfd_enable(smp_processor_id());
}

static int __snp_enable(unsigned int cpu)
{
	u64 val;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	rdmsrl(MSR_AMD64_SYSCFG, val);

	val |= MSR_AMD64_SYSCFG_SNP_EN;
	val |= MSR_AMD64_SYSCFG_SNP_VMPL_EN;

	wrmsrl(MSR_AMD64_SYSCFG, val);

	return 0;
}

static __init void snp_enable(void *arg)
{
	__snp_enable(smp_processor_id());
}

#define RMP_ADDR_MASK GENMASK_ULL(51, 13)

bool snp_probe_rmptable_info(void)
{
	u64 max_rmp_pfn, calc_rmp_sz, rmp_sz, rmp_base, rmp_end;

	rdmsrl(MSR_AMD64_RMP_BASE, rmp_base);
	rdmsrl(MSR_AMD64_RMP_END, rmp_end);

	if (!(rmp_base & RMP_ADDR_MASK) || !(rmp_end & RMP_ADDR_MASK)) {
		pr_err("Memory for the RMP table has not been reserved by BIOS\n");
		return false;
	}

	if (rmp_base > rmp_end) {
		pr_err("RMP configuration not valid: base=%#llx, end=%#llx\n", rmp_base, rmp_end);
		return false;
	}

	rmp_sz = rmp_end - rmp_base + 1;

	/*
	 * Calculate the amount the memory that must be reserved by the BIOS to
	 * address the whole RAM, including the bookkeeping area. The RMP itself
	 * must also be covered.
	 */
	max_rmp_pfn = max_pfn;
	if (PHYS_PFN(rmp_end) > max_pfn)
		max_rmp_pfn = PHYS_PFN(rmp_end);

	calc_rmp_sz = (max_rmp_pfn << 4) + RMPTABLE_CPU_BOOKKEEPING_SZ;

	if (calc_rmp_sz > rmp_sz) {
		pr_err("Memory reserved for the RMP table does not cover full system RAM (expected 0x%llx got 0x%llx)\n",
		       calc_rmp_sz, rmp_sz);
		return false;
	}

	probed_rmp_base = rmp_base;
	probed_rmp_size = rmp_sz;

	pr_info("RMP table physical range [0x%016llx - 0x%016llx]\n",
		probed_rmp_base, probed_rmp_base + probed_rmp_size - 1);

	return true;
}

/*
 * Do the necessary preparations which are verified by the firmware as
 * described in the SNP_INIT_EX firmware command description in the SNP
 * firmware ABI spec.
 */
static int __init snp_rmptable_init(void)
{
	void *rmptable_start;
	u64 rmptable_size;
	u64 val;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return 0;

	if (!amd_iommu_snp_en)
		return 0;

	if (!probed_rmp_size)
		goto nosnp;

	rmptable_start = memremap(probed_rmp_base, probed_rmp_size, MEMREMAP_WB);
	if (!rmptable_start) {
		pr_err("Failed to map RMP table\n");
		return 1;
	}

	/*
	 * Check if SEV-SNP is already enabled, this can happen in case of
	 * kexec boot.
	 */
	rdmsrl(MSR_AMD64_SYSCFG, val);
	if (val & MSR_AMD64_SYSCFG_SNP_EN)
		goto skip_enable;

	memset(rmptable_start, 0, probed_rmp_size);

	/* Flush the caches to ensure that data is written before SNP is enabled. */
	wbinvd_on_all_cpus();

	/* MtrrFixDramModEn must be enabled on all the CPUs prior to enabling SNP. */
	on_each_cpu(mfd_enable, NULL, 1);

	on_each_cpu(snp_enable, NULL, 1);

skip_enable:
	rmptable_start += RMPTABLE_CPU_BOOKKEEPING_SZ;
	rmptable_size = probed_rmp_size - RMPTABLE_CPU_BOOKKEEPING_SZ;

	rmptable = (struct rmpentry *)rmptable_start;
	rmptable_max_pfn = rmptable_size / sizeof(struct rmpentry) - 1;

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "x86/rmptable_init:online", __snp_enable, NULL);

	return 0;

nosnp:
	setup_clear_cpu_cap(X86_FEATURE_SEV_SNP);
	return -ENOSYS;
}

/*
 * This must be called after the IOMMU has been initialized.
 */
device_initcall(snp_rmptable_init);

static struct rmpentry *get_rmpentry(u64 pfn)
{
	if (WARN_ON_ONCE(pfn > rmptable_max_pfn))
		return ERR_PTR(-EFAULT);

	return &rmptable[pfn];
}

static struct rmpentry *__snp_lookup_rmpentry(u64 pfn, int *level)
{
	struct rmpentry *large_entry, *entry;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return ERR_PTR(-ENODEV);

	entry = get_rmpentry(pfn);
	if (IS_ERR(entry))
		return entry;

	/*
	 * Find the authoritative RMP entry for a PFN. This can be either a 4K
	 * RMP entry or a special large RMP entry that is authoritative for a
	 * whole 2M area.
	 */
	large_entry = get_rmpentry(pfn & PFN_PMD_MASK);
	if (IS_ERR(large_entry))
		return large_entry;

	*level = RMP_TO_PG_LEVEL(large_entry->pagesize);

	return entry;
}

int snp_lookup_rmpentry(u64 pfn, bool *assigned, int *level)
{
	struct rmpentry *e;

	e = __snp_lookup_rmpentry(pfn, level);
	if (IS_ERR(e))
		return PTR_ERR(e);

	*assigned = !!e->assigned;
	return 0;
}
EXPORT_SYMBOL_GPL(snp_lookup_rmpentry);

/*
 * Dump the raw RMP entry for a particular PFN. These bits are documented in the
 * PPR for a particular CPU model and provide useful information about how a
 * particular PFN is being utilized by the kernel/firmware at the time certain
 * unexpected events occur, such as RMP faults.
 */
static void dump_rmpentry(u64 pfn)
{
	u64 pfn_i, pfn_end;
	struct rmpentry *e;
	int level;

	e = __snp_lookup_rmpentry(pfn, &level);
	if (IS_ERR(e)) {
		pr_err("Failed to read RMP entry for PFN 0x%llx, error %ld\n",
		       pfn, PTR_ERR(e));
		return;
	}

	if (e->assigned) {
		pr_warn("PFN 0x%llx, RMP entry: [0x%016llx - 0x%016llx]\n",
			pfn, e->lo, e->hi);
		if (!amd_iommu_snp_debug)
			return;
	}

	/*
	 * If the RMP entry for a particular PFN is not in an assigned state,
	 * then it is sometimes useful to get an idea of whether or not any RMP
	 * entries for other PFNs within the same 2MB region are assigned, since
	 * those too can affect the ability to access a particular PFN in
	 * certain situations, such as when the PFN is being accessed via a 2MB
	 * mapping in the host page table.
	 */
	pfn_i = ALIGN_DOWN(pfn, PTRS_PER_PMD);
	pfn_end = pfn_i + PTRS_PER_PMD;

	pr_warn("PFN 0x%llx unassigned, dumping non-zero entries in 2M PFN region: [0x%llx - 0x%llx]\n",
		pfn, pfn_i, pfn_end);

	while (pfn_i < pfn_end) {
		e = __snp_lookup_rmpentry(pfn_i, &level);
		if (IS_ERR(e)) {
			pr_err("Error %ld reading RMP entry for PFN 0x%llx\n",
			       PTR_ERR(e), pfn_i);
			pfn_i++;
			continue;
		}

		if (e->lo || e->hi)
			pr_warn("PFN: 0x%llx, [0x%016llx - 0x%016llx]\n", pfn_i, e->lo, e->hi);
		pfn_i++;
	}
}

void snp_dump_hva_rmpentry(unsigned long hva)
{
	unsigned long paddr;
	unsigned int level;
	pgd_t *pgd;
	pte_t *pte;

	pgd = __va(read_cr3_pa());
	pgd += pgd_index(hva);
	pte = lookup_address_in_pgd(pgd, hva, &level);

	if (!pte) {
		pr_err("Can't dump RMP entry for HVA %lx: no PTE/PFN found\n", hva);
		return;
	}

	paddr = PFN_PHYS(pte_pfn(*pte)) | (hva & ~page_level_mask(level));
	dump_rmpentry(PHYS_PFN(paddr));
}

static bool soft_rmptable __ro_after_init;

/*
 * Test if the rmptable needs to be managed by software and is not maintained by
 * (virtualized) hardware.
 */
bool snp_soft_rmptable(void)
{
	return soft_rmptable;
}

void __init snp_set_soft_rmptable(void)
{
	soft_rmptable = true;
}

static bool virt_snp_msr(void)
{
	return boot_cpu_has(X86_FEATURE_NESTED_VIRT_SNP_MSR);
}

/*
 * This version of psmash is not implemented in hardware but always
 * traps to L0 hypervisor. It doesn't follow usual wrmsr conventions.
 * Inputs:
 *   rax: 2MB aligned GPA
 * Outputs:
 *   rax: psmash return code
 */
static u64 virt_psmash(u64 paddr)
{
	int ret;

	asm volatile(
		"wrmsr\n\t"
		: "=a"(ret)
		: "a"(paddr), "c"(MSR_AMD64_VIRT_PSMASH)
		: "memory", "cc"
	);
	return ret;
}

static void snp_update_rmptable_psmash(u64 pfn)
{
	int level;
	struct rmpentry entry;
	int ret = __snp_lookup_rmpentry(pfn, &entry, &level);

	if (WARN_ON(ret))
		return;

	if (level == PG_LEVEL_2M) {
		int i;

		entry.pagesize = RMP_PG_SIZE_4K;
		rmptable_start[pfn] = entry;
		for (i = 1; i < PTRS_PER_PMD; i++) {
			struct rmpentry *it = &rmptable_start[pfn + i];
			*it = entry;
			it->gpa = entry.gpa + i * PAGE_SIZE;
		}
	}
}


/*
 * PSMASH a 2MB aligned page into 4K pages in the RMP table while preserving the
 * Validated bit.
 */
int psmash(u64 pfn)
{
	unsigned long paddr = pfn << PAGE_SHIFT;
	int ret;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return -ENODEV;

	if (!pfn_valid(pfn))
		return -EINVAL;

	if (virt_snp_msr()) {
		ret = virt_psmash(paddr);
		if (!ret && snp_soft_rmptable())
			snp_update_rmptable_psmash(pfn);
	} else {
		/* Binutils version 2.36 supports the PSMASH mnemonic. */
		asm volatile(".byte 0xF3, 0x0F, 0x01, 0xFF"
			      : "=a" (ret)
			      : "a" (paddr)
			      : "memory", "cc");
	}

	return ret;
}
EXPORT_SYMBOL_GPL(psmash);

static int restore_direct_map(u64 pfn, int npages)
{
	int i, ret = 0;

	for (i = 0; i < npages; i++) {
		ret = set_direct_map_default_noflush(pfn_to_page(pfn + i));
		if (ret)
			break;
	}

	if (ret)
		pr_warn("Failed to restore direct map for pfn 0x%llx, ret: %d\n",
			pfn + i, ret);

	return ret;
}

static int invalidate_direct_map(u64 pfn, int npages)
{
	int i, ret = 0;

	for (i = 0; i < npages; i++) {
		ret = set_direct_map_invalid_noflush(pfn_to_page(pfn + i));
		if (ret)
			break;
	}

	if (ret) {
		pr_warn("Failed to invalidate direct map for pfn 0x%llx, ret: %d\n",
			pfn + i, ret);
		restore_direct_map(pfn, i);
	}

	return ret;
}

 * This version of rmpupdate is not implemented in hardware but always
 * traps to L0 hypervisor. It doesn't follow usual wrmsr conventions.
 * Inputs:
 *   rax: 4KB aligned GPA
 *   rdx: bytes 7:0 of new rmp entry
 *   r8:  bytes 15:8 of new rmp entry
 * Outputs:
 *   rax: rmpupdate return code
 */
static u64 virt_rmpupdate(unsigned long paddr, struct rmp_state *val)
{
	int ret;
	register u64 hi asm("r8") = ((u64 *)val)[1];
	register u64 lo asm("rdx") = ((u64 *)val)[0];

	asm volatile(
		"wrmsr\n\t"
		: "=a"(ret)
		: "a"(paddr), "c"(MSR_AMD64_VIRT_RMPUPDATE), "r"(lo), "r"(hi)
		: "memory", "cc"
	);
	return ret;
}

static void snp_update_rmptable_rmpupdate(u64 pfn, int level, struct rmp_state *val)
{
	int prev_level;
	struct rmpentry entry;
	int ret = __snp_lookup_rmpentry(pfn, &entry, &prev_level);

	if (WARN_ON(ret))
		return;

	if (level > PG_LEVEL_4K) {
		int i;
		struct rmpentry tmp_rmp = {
			.assigned = val->assigned,
		};
		WARN_ON((pfn + PTRS_PER_PMD) > rmptable_max_pfn);
		for (i = 1; i < PTRS_PER_PMD; i++)
			rmptable_start[pfn + i] = tmp_rmp;
	}
	if (!val->assigned) {
		memset(&entry, 0, sizeof(entry));
	} else {
		entry.assigned = val->assigned;
		entry.pagesize = val->pagesize;
		entry.immutable = val->immutable;
		entry.gpa = val->gpa;
		entry.asid = val->asid;
	}
	rmptable_start[pfn] = entry;
}

/*
 * It is expected that those operations are seldom enough so that no mutual
 * exclusion of updaters is needed and thus the overlap error condition below
 * should happen very seldomly and would get resolved relatively quickly by
 * the firmware.
 *
 * If not, one could consider introducing a mutex or so here to sync concurrent
 * RMP updates and thus diminish the amount of cases where firmware needs to
 * lock 2M ranges to protect against concurrent updates.
 *
 * The optimal solution would be range locking to avoid locking disjoint
 * regions unnecessarily but there's no support for that yet.
 */
static int rmpupdate(u64 pfn, struct rmp_state *state)
{
	unsigned long paddr = pfn << PAGE_SHIFT;
	int ret, level, npages;
	int retries = 2000000;

	if (!cpu_feature_enabled(X86_FEATURE_SEV_SNP))
		return -ENODEV;

	if (amd_iommu_snp_debug) {
		pr_warn("dumping PFN 0x%llx RMP entry state prior to RMPUPDATE...\n", pfn);
		dump_rmpentry(pfn);
	}

	level = RMP_TO_PG_LEVEL(state->pagesize);
	npages = page_level_size(level) / PAGE_SIZE;

	/*
	 * If the kernel uses a 2MB directmap mapping to write to an address,
	 * and that 2MB range happens to contain a 4KB page that set to private
	 * in the RMP table, an RMP #PF will trigger and cause a host crash.
	 *
	 * Prevent this by removing pages from the directmap prior to setting
	 * them as private in the RMP table.
	 */
	if (state->assigned && invalidate_direct_map(pfn, npages))
		return -EFAULT;

	do {
		if (virt_snp_msr()) {
			ret = virt_rmpupdate(paddr, val);
			if (!ret && snp_soft_rmptable())
				snp_update_rmptable_rmpupdate(pfn, level, val);
		} else {
			/* Binutils version 2.36 supports the RMPUPDATE mnemonic. */
			asm volatile(".byte 0xF2, 0x0F, 0x01, 0xFE"
				     : "=a" (ret)
				     : "a" (paddr), "c" ((unsigned long)state)
				     : "memory", "cc");
		}
		if (amd_iommu_snp_debug && ret)
			retries--;
	} while (ret == RMPUPDATE_FAIL_OVERLAP && retries);

	if (ret) {
		pr_err("RMPUPDATE failed for PFN %llx, pg_level: %d, ret: %d\n",
		       pfn, level, ret);
		dump_rmpentry(pfn);
		dump_stack();
		return -EFAULT;
	}

	if (!state->assigned && restore_direct_map(pfn, npages))
		return -EFAULT;

	return 0;
}

/* Transition a page to guest-owned/private state in the RMP table. */
int rmp_make_private(u64 pfn, u64 gpa, enum pg_level level, int asid, bool immutable)
{
	struct rmp_state state;

	memset(&state, 0, sizeof(state));
	state.assigned = 1;
	state.asid = asid;
	state.immutable = immutable;
	state.gpa = gpa;
	state.pagesize = PG_LEVEL_TO_RMP(level);

	return rmpupdate(pfn, &state);
}
EXPORT_SYMBOL_GPL(rmp_make_private);

/* Transition a page to hypervisor-owned/shared state in the RMP table. */
int rmp_make_shared(u64 pfn, enum pg_level level)
{
	struct rmp_state state;

	memset(&state, 0, sizeof(state));
	state.pagesize = PG_LEVEL_TO_RMP(level);

	return rmpupdate(pfn, &state);
}
EXPORT_SYMBOL_GPL(rmp_make_shared);

void snp_leak_pages(u64 pfn, unsigned int npages)
{
	struct page *page = pfn_to_page(pfn);

	pr_debug("%s: leaking PFN range 0x%llx-0x%llx\n", __func__, pfn, pfn + npages);

	spin_lock(&snp_leaked_pages_list_lock);
	while (npages--) {
		/*
		 * Reuse the page's buddy list for chaining into the leaked
		 * pages list. This page should not be on a free list currently
		 * and is also unsafe to be added to a free list.
		 */
		if (likely(!PageCompound(page)) ||
		    (PageHead(page) && compound_nr(page) <= npages))
			/*
			 * Skip inserting tail pages of compound page as
			 * page->buddy_list of tail pages is not usable.
			 */
			list_add_tail(&page->buddy_list, &snp_leaked_pages_list);
		dump_rmpentry(pfn);
		snp_nr_leaked_pages++;
		pfn++;
		page++;
	}
	spin_unlock(&snp_leaked_pages_list_lock);
}
EXPORT_SYMBOL_GPL(snp_leak_pages);

u64 snp_config_transaction_start(void)
{
	u64 id;

	mutex_lock(&snp_transaction_lock);
	snp_transaction_pending = true;
	id = ++snp_transaction_id;
	mutex_unlock(&snp_transaction_lock);

	return id;
}
EXPORT_SYMBOL_GPL(snp_config_transaction_start);

u64 snp_config_transaction_end(void)
{
	u64 id;

	mutex_lock(&snp_transaction_lock);
	snp_transaction_pending = false;
	id = snp_transaction_id;
	mutex_unlock(&snp_transaction_lock);

	return id;
}
EXPORT_SYMBOL_GPL(snp_config_transaction_end);

u64 snp_config_transaction_get_id(void)
{
	return snp_transaction_id;
}
EXPORT_SYMBOL_GPL(snp_config_transaction_get_id);

bool snp_config_transaction_is_stale(u64 id)
{
	bool stale = false;

	mutex_lock(&snp_transaction_lock);
	if (snp_transaction_pending ||
	    id != snp_transaction_id)
		stale = true;
	mutex_unlock(&snp_transaction_lock);

	return stale;
}
EXPORT_SYMBOL_GPL(snp_config_transaction_is_stale);

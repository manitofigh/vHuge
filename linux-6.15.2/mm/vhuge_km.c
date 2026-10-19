// SPDX-License-Identifier: GPL-2.0
/*
 * vHuge: daemon-filled pool of verified real PMD-sized THPs.
 *
 * Userspace probes its own THPs, reports the virtual addresses classified as
 * REAL, then unmaps them. The free path captures only those marked PMD folios
 * into this pool. THP fault and khugepaged allocation paths consume the pool
 * before falling back to the normal allocator.
 *
 * The file holds the pool and its allocator hooks, the /dev/vhuge_pool
 * interface the daemon uses to mark pages and to revalidate pooled pages,
 * the optional DAMON classification of hot huge pages, the in-kernel prober
 * and the migration of hot fake huge pages onto pooled real ones, and the
 * /proc/vhuge and /sys/kernel/vhuge_pool interfaces.
 */
#include <linux/capability.h>
#include <linux/compat.h>
#ifdef CONFIG_VHUGE_DAMON
#include <linux/damon.h>
#endif
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/huge_mm.h>
#include <linux/ioctl.h>
#include <linux/jiffies.h>
#include <linux/kthread.h>
#include <linux/kobject.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/migrate.h>
#include <linux/page-flags.h>
#include <linux/pfn_t.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/swap.h>
#include <linux/mempolicy.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/vhuge.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <asm/barrier.h>
#include <asm/msr.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#include "internal.h"

#define VHUGE_PROC_NAME		"vhuge"
#define VHUGE_SYSFS_NAME	"vhuge_pool"
#define VHUGE_DEV_NAME		"vhuge_pool"
#define VHUGE_TOOL_COMM		"vhuge"
#define VHUGE_POOL_TARGET_DEF	512U
#define VHUGE_POOL_LOW_PCT_DEF	2U
#define VHUGE_POOL_MAX_DEF	8192U
#define VHUGE_MARK_ADDRS_MAX	4096U
#define VHUGE_VALIDATE_MAX	512U
#define VHUGE_DAMON_SAMPLE_US_DEF	20000UL
#define VHUGE_DAMON_AGGR_US_DEF	500000UL
#define VHUGE_DAMON_UPDATE_US_DEF	1000000UL
#define VHUGE_DAMON_MIN_REGIONS_DEF	128UL
#define VHUGE_DAMON_MAX_REGIONS_DEF	2048UL
#define VHUGE_DAMON_HOT_THRESHOLD_BP_DEF	5000U
#define VHUGE_DAMON_HOT_PMD_MAX_DEF	8192U
#define VHUGE_DAMON_CLASSIFY_INTERVAL_SEC	1U
#define VHUGE_DAMON_TARGET_MAX_DEF	128U
#define VHUGE_DAMON_MIGRATION_RESERVE_DEF	32U
#define VHUGE_DAMON_PROBE_THRESHOLD_DEF	55U
#define VHUGE_DAMON_PROBE_SAMPLES_DEF	5U
#define VHUGE_DAMON_PROBE_SAMPLES_MAX	32U
#define VHUGE_DAMON_PROBE_CALIB_SAMPLES	2000U
#define VHUGE_DAMON_PROBE_MAX_PER_SCAN_DEF	16U
#define VHUGE_DAMON_PROBE_COOLDOWN_SEC_DEF	60U
#define VHUGE_DAMON_MIGRATE_MAX_PER_SCAN_DEF	16U
#define VHUGE_DAMON_PRESSURE_FAILURES_DEF	3U
#define VHUGE_DAMON_STOP_WATERMARK_PCT_DEF	50U
#define VHUGE_DAMON_MIN_RUNTIME_SEC_DEF		30U
#define VHUGE_DAMON_FIXED_SCOUT_AGGRS_DEF	3U
#define VHUGE_IOC_MAGIC		'V'
#define VHUGE_DAEMON_STATUS_REFILL_ATTEMPTED	BIT(0)
#define VHUGE_DAEMON_STATUS_REFILL_FILL_TO	BIT(1)
#define VHUGE_DAEMON_STATUS_KNOWN_FLAGS \
	(VHUGE_DAEMON_STATUS_REFILL_ATTEMPTED | \
	 VHUGE_DAEMON_STATUS_REFILL_FILL_TO)

struct vhuge_mark_addrs {
	__u64 addrs;
	__u32 nr_addrs;
	__u32 flags;
};

#define VHUGE_IOC_MARK_REAL_ADDRS \
	_IOW(VHUGE_IOC_MAGIC, 1, struct vhuge_mark_addrs)

struct vhuge_validate_batch {
	__u32 max_entries;
	__u32 nr_entries;
	__u32 flags;
};

struct vhuge_validate_result {
	__u32 slot;
	__u32 is_real;
};

struct vhuge_validate_report {
	__u64 results;
	__u32 nr_results;
	__u32 flags;
};

struct vhuge_daemon_status {
	__u32 next_refill_check_sec;
	__u32 next_validation_sec;
	__u32 last_refill_wanted;
	__u32 last_refill_captured;
	__u32 last_refill_success_pct;
	__u32 last_validation_batch;
	__u32 flags;
	__u32 last_refill_seq;
};

#define VHUGE_IOC_GET_VALIDATE_BATCH \
	_IOWR(VHUGE_IOC_MAGIC, 2, struct vhuge_validate_batch)
#define VHUGE_IOC_REPORT_VALIDATE \
	_IOW(VHUGE_IOC_MAGIC, 3, struct vhuge_validate_report)
#define VHUGE_IOC_SET_DAEMON_STATUS \
	_IOW(VHUGE_IOC_MAGIC, 4, struct vhuge_daemon_status)

struct vhuge_pool_entry {
	struct list_head list;
	struct page *page;
	unsigned long pfn;
	int nid;
	u32 validation_slot;
	bool allocated;
};

struct vhuge_file_ctx {
	struct list_head validating;
	struct mutex validating_lock;
	refcount_t refs;
	struct mm_struct *mm;
	u32 validating_count;
	u32 mapping_count;
	bool fd_closed;
	bool daemon_registered;
};

struct vhuge_mm_owner {
	struct list_head list;
	struct mm_struct *mm;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	u32 real_hps;
	u64 thp_fault_hps;
	u64 khugepaged_hps;
	u64 damon_probe_hps;
	u64 migrated_hps;
};

struct vhuge_owned_hp {
	enum vhuge_hp_source source;
	struct vhuge_mm_owner *owner;
};

struct vhuge_proc_owner {
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	u32 real_hps;
	u64 thp_fault_hps;
	u64 khugepaged_hps;
	u64 damon_probe_hps;
	u64 migrated_hps;
};

struct vhuge_stats {
	u64 mark_success;
	u64 mark_fail;
	u64 captured;
	u64 consumed;
	u64 alloc_empty;
	u64 alloc_local;
	u64 alloc_remote;
	u64 alloc_local_miss;
	u64 released;
	u64 thp_fault_allocs;
	u64 khugepaged_allocs;
	u64 daemon_pool_bypass;
	u64 tool_pool_bypass;
	u64 owned_current;
	u64 ownership_lost;
	u64 ownership_splits;
	u64 validation_checked;
	u64 validation_failed;
	u64 validation_returned;
	u64 validation_released;
	u64 validation_quarantined;
	u64 damon_policy_refill_failures;
	u64 damon_policy_refill_recovered;
	u64 damon_policy_fill_to_ignored;
	u64 damon_policy_started;
	u64 damon_policy_stopped;
	u64 damon_scout_started;
	u64 damon_scout_recovered;
	u64 damon_targets_registered;
	u64 damon_targets_reaped;
	u64 damon_target_rebuilds;
	u64 damon_alloc_hot;
	u64 damon_alloc_cold_bypass;
	u64 damon_alloc_reserve_bypass;
	u64 damon_fixed_armed;
	u64 damon_fixed_activated;
	u64 damon_budget_reserved;
	u64 damon_budget_released;
	u64 damon_budget_exhausted;
	u64 damon_aggregations;
	u64 damon_hot_pmd_ranges;
	u64 damon_hot_table_full;
	u64 damon_last_update_us;
	u64 damon_max_update_us;
	u64 damon_scan_passes;
	u64 damon_scan_slots_checked;
	u64 damon_scan_transhuge;
	u64 damon_scan_hot_mapped;
	u64 damon_scan_hot_real;
	u64 damon_scan_hot_unknown;
	u64 damon_scan_hot_fake;
	u64 damon_scan_last_checked;
	u64 damon_scan_last_transhuge;
	u64 damon_scan_last_hot_mapped;
	u64 damon_scan_last_hot_real;
	u64 damon_scan_last_hot_unknown;
	u64 damon_scan_last_hot_fake;
	u64 damon_scan_skipped_no_hot;
	u64 damon_last_scan_us;
	u64 damon_max_scan_us;
	u64 damon_probe_queued;
	u64 damon_probe_attempted;
	u64 damon_probe_real;
	u64 damon_probe_fake;
	u64 damon_probe_failed;
	u64 damon_probe_faulted;
	u64 damon_probe_cooldown_skip;
	u64 damon_probe_inflight_skip;
	u64 damon_probe_budget_hit;
	u64 damon_probe_last_us;
	u64 damon_probe_max_us;
	u64 damon_probe_last_median_cycles;
	u64 damon_probe_max_median_cycles;
	u64 damon_migrate_queued;
	u64 damon_migrate_attempted;
	u64 damon_migrate_succeeded;
	u64 damon_migrate_failed;
	u64 damon_migrate_no_pool;
	u64 damon_migrate_revalidate_failed;
	u64 damon_migrate_isolate_failed;
	u64 damon_migrate_pages_failed;
	u64 damon_migrate_account_failed;
	u64 damon_migrate_limit_hit;
	u64 damon_migrate_last_total_us;
	u64 damon_migrate_max_total_us;
	u64 damon_migrate_last_isolate_us;
	u64 damon_migrate_max_isolate_us;
	u64 damon_migrate_last_pool_us;
	u64 damon_migrate_max_pool_us;
	u64 damon_migrate_last_migrate_us;
	u64 damon_migrate_max_migrate_us;
	u64 damon_migrate_last_account_us;
	u64 damon_migrate_max_account_us;
};

#ifdef CONFIG_VHUGE_DAMON
static void vhuge_damon_classify_workfn(struct work_struct *work);
static void vhuge_damon_rebuild_workfn(struct work_struct *work);
static bool vhuge_damon_vma_allowed(struct vm_area_struct *vma);
static void vhuge_damon_policy_reset_locked(bool stop_running);
static void vhuge_damon_policy_update(const struct vhuge_daemon_status *status);
#endif
static struct vhuge_pool_entry *vhuge_take_pool_entry(int nid,
						      bool count_empty);

static DEFINE_XARRAY_FLAGS(vhuge_capture_xa, XA_FLAGS_LOCK_IRQ);
static DEFINE_XARRAY_FLAGS(vhuge_consumed_xa, XA_FLAGS_LOCK_IRQ);
static DEFINE_XARRAY_FLAGS(vhuge_owned_xa, XA_FLAGS_LOCK_IRQ);
static LIST_HEAD(vhuge_pool);
static LIST_HEAD(vhuge_quarantine);
static LIST_HEAD(vhuge_owner_list);
static DEFINE_SPINLOCK(vhuge_pool_lock);
static DEFINE_SPINLOCK(vhuge_quarantine_lock);
static DEFINE_SPINLOCK(vhuge_owner_lock);
static DEFINE_MUTEX(vhuge_enable_lock);

static struct proc_dir_entry *vhuge_proc;
static struct kobject *vhuge_kobj;
static bool vhuge_enabled = true;
static u32 vhuge_proc_verbosity;
static u32 vhuge_pool_target = VHUGE_POOL_TARGET_DEF;
static u32 vhuge_pool_low;
static u32 vhuge_pool_max = VHUGE_POOL_MAX_DEF;
static u32 vhuge_pool_fill_to;
static u32 vhuge_pool_count;
static atomic_t vhuge_validation_in_progress = ATOMIC_INIT(0);
static atomic_t vhuge_daemon_open = ATOMIC_INIT(0);
static struct mm_struct *vhuge_daemon_mm;
static struct vhuge_stats vhuge_stats;
static struct vhuge_daemon_status vhuge_daemon_status;

/*
 * Choose the refill trigger once, from the free RAM available when vHuge
 * initializes.  The result is expressed in PMD-sized THPs, since that is
 * the unit used by the pool and by the daemon.
 */
static u32 vhuge_boot_low_watermark(void)
{
	u64 free_pages = nr_free_pages();
	u64 low_pages;

	low_pages = DIV_ROUND_UP_ULL(free_pages * VHUGE_POOL_LOW_PCT_DEF,
				    100ULL * HPAGE_PMD_NR);
	return min_t(u64, low_pages, VHUGE_POOL_TARGET_DEF);
}

#ifdef CONFIG_VHUGE_DAMON
struct vhuge_monitor_target {
	struct list_head list;
	struct pid *pid;
	struct xarray hot_pmds;
};

enum vhuge_damon_mode {
	VHUGE_DAMON_IDLE = 0,
	VHUGE_DAMON_SCOUT,
	VHUGE_DAMON_PRESSURE,
};

enum vhuge_damon_policy_mode {
	VHUGE_DAMON_POLICY_AUTO = 0,
	VHUGE_DAMON_POLICY_FIXED_BUDGET,
};

static LIST_HEAD(vhuge_monitor_targets);
static DEFINE_MUTEX(vhuge_monitor_lock);
static u32 vhuge_monitor_target_count;
static bool vhuge_damon_targets_dirty;

static DEFINE_MUTEX(vhuge_damon_lock);
static DEFINE_SPINLOCK(vhuge_damon_budget_lock);
static DEFINE_XARRAY_FLAGS(vhuge_damon_budget_xa, XA_FLAGS_LOCK_IRQ);
static struct damon_ctx *vhuge_damon_ctx;
static struct workqueue_struct *vhuge_damon_wq;
static DECLARE_DELAYED_WORK(vhuge_damon_classify_work,
			    vhuge_damon_classify_workfn);
static DECLARE_DELAYED_WORK(vhuge_damon_rebuild_work,
			    vhuge_damon_rebuild_workfn);
static bool vhuge_damon_enabled = true;
static bool vhuge_damon_running;
static enum vhuge_damon_mode vhuge_damon_mode = VHUGE_DAMON_IDLE;
static enum vhuge_damon_policy_mode vhuge_damon_policy_mode =
	VHUGE_DAMON_POLICY_AUTO;
static unsigned long vhuge_damon_sample_us = VHUGE_DAMON_SAMPLE_US_DEF;
static unsigned long vhuge_damon_aggr_us = VHUGE_DAMON_AGGR_US_DEF;
static unsigned long vhuge_damon_update_us = VHUGE_DAMON_UPDATE_US_DEF;
static unsigned long vhuge_damon_min_regions = VHUGE_DAMON_MIN_REGIONS_DEF;
static unsigned long vhuge_damon_max_regions = VHUGE_DAMON_MAX_REGIONS_DEF;
static u32 vhuge_damon_hot_threshold_bp = VHUGE_DAMON_HOT_THRESHOLD_BP_DEF;
static u32 vhuge_damon_hot_pmd_max = VHUGE_DAMON_HOT_PMD_MAX_DEF;
static u32 vhuge_damon_migration_reserve =
	VHUGE_DAMON_MIGRATION_RESERVE_DEF;
static u32 vhuge_damon_reserve_floor;
static u32 vhuge_damon_probe_threshold_cycles =
	VHUGE_DAMON_PROBE_THRESHOLD_DEF;
static u32 vhuge_damon_probe_samples = VHUGE_DAMON_PROBE_SAMPLES_DEF;
/* Calibrate the probe threshold in the kernel instead of using poold's. */
static bool vhuge_damon_probe_self_calibrate = true;
static bool vhuge_damon_probe_calibrated;
static u32 vhuge_damon_probe_calib_hit, vhuge_damon_probe_calib_miss;
static u32 vhuge_damon_probe_max_per_scan =
	VHUGE_DAMON_PROBE_MAX_PER_SCAN_DEF;
static u32 vhuge_damon_probe_cooldown_sec =
	VHUGE_DAMON_PROBE_COOLDOWN_SEC_DEF;
static u32 vhuge_damon_migrate_max_per_scan =
	VHUGE_DAMON_MIGRATE_MAX_PER_SCAN_DEF;
static u32 vhuge_damon_pressure_failures =
	VHUGE_DAMON_PRESSURE_FAILURES_DEF;
static u32 vhuge_damon_stop_watermark_pct =
	VHUGE_DAMON_STOP_WATERMARK_PCT_DEF;
static u32 vhuge_damon_min_runtime_sec =
	VHUGE_DAMON_MIN_RUNTIME_SEC_DEF;
static u32 vhuge_damon_fixed_scout_aggrs =
	VHUGE_DAMON_FIXED_SCOUT_AGGRS_DEF;
static u32 vhuge_damon_fixed_aggr_count;
static u32 vhuge_damon_hot_budget_pages;
/* Fixed-budget comparison without DAMON: spend the budget in allocation order. */
static bool vhuge_damon_budget_fcfs;
static u32 vhuge_damon_hot_budget_in_use;
static u32 vhuge_damon_refill_failures;
static u32 vhuge_damon_last_refill_seq;
static unsigned long vhuge_damon_started_jiffies;

enum vhuge_probe_state {
	VHUGE_PROBE_INFLIGHT = 1,
	VHUGE_PROBE_FAKE,
};

struct vhuge_probe_record {
	enum vhuge_probe_state state;
	unsigned long expires;
};

/* A hot huge page queued for the probe or the migration worker. */
struct vhuge_damon_candidate {
	struct kthread_work work;
	struct mm_struct *mm;
	struct folio *folio;
	unsigned long addr;
	unsigned long hp_index;
};

struct vhuge_migrate_target {
	struct vhuge_pool_entry *entry;
	struct folio *folio;
	bool handed;
};

struct vhuge_damon_scan_result {
	u64 slots_checked;
	u64 transhuge;
	u64 hot_mapped;
	u64 hot_real;
	u64 hot_unknown;
	u64 hot_fake;
	u32 probes_queued;
	u32 migrations_queued;
	bool probe_budget_hit;
	bool migrate_limit_hit;
};

struct vhuge_hot_candidate {
	struct pid *pid;
	unsigned long addr;
	u32 score;
};

static DEFINE_XARRAY(vhuge_probe_xa);
static DEFINE_MUTEX(vhuge_probe_lock);
static struct kthread_worker *vhuge_probe_worker;
static atomic_t vhuge_probe_inflight = ATOMIC_INIT(0);
static DEFINE_XARRAY(vhuge_migrate_xa);
static DEFINE_MUTEX(vhuge_migrate_lock);
static struct kthread_worker *vhuge_migrate_worker;
static atomic_t vhuge_migrate_inflight = ATOMIC_INIT(0);
#endif

static unsigned long pfn_to_hp_index(unsigned long pfn)
{
	return pfn >> HPAGE_PMD_ORDER;
}

#ifdef CONFIG_VHUGE_DAMON
static void update_timing(u64 elapsed, u64 *last, u64 *max)
{
	WRITE_ONCE(*last, elapsed);
	if (elapsed > READ_ONCE(*max))
		WRITE_ONCE(*max, elapsed);
}

static u32 vhuge_damon_budget_in_use(void)
{
	unsigned long flags;
	u32 in_use;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	in_use = vhuge_damon_hot_budget_in_use;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	return in_use;
}

static bool vhuge_damon_budget_has_room(void)
{
	unsigned long flags;
	bool room;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	room = vhuge_damon_hot_budget_in_use <
		vhuge_damon_hot_budget_pages;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	return room;
}

/*
 * Reserve one page of the fixed budget. The first-come comparison spends it
 * at allocation time; DAMON placement only while it runs in pressure mode.
 */
static bool vhuge_damon_budget_try_reserve(bool fcfs)
{
	unsigned long flags;
	bool reserved = false;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	if ((fcfs ||
	     (READ_ONCE(vhuge_enabled) &&
	      READ_ONCE(vhuge_damon_enabled) &&
	      READ_ONCE(vhuge_damon_running) &&
	      READ_ONCE(vhuge_damon_policy_mode) ==
			VHUGE_DAMON_POLICY_FIXED_BUDGET &&
	      READ_ONCE(vhuge_damon_mode) == VHUGE_DAMON_PRESSURE)) &&
	    vhuge_damon_hot_budget_in_use < vhuge_damon_hot_budget_pages) {
		vhuge_damon_hot_budget_in_use++;
		reserved = true;
	}
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	if (reserved)
		vhuge_stats.damon_budget_reserved++;
	else
		vhuge_stats.damon_budget_exhausted++;
	return reserved;
}

/*
 * The first-come budget is only spent on tasks that run under MPOL_BIND, so
 * that unrelated system tasks cannot consume the experiment's budget.
 */
static bool vhuge_current_is_membound(void)
{
#ifdef CONFIG_NUMA
	struct mempolicy *pol = current->mempolicy;

	return pol && pol->mode == MPOL_BIND;
#else
	return false;
#endif
}

static void vhuge_damon_budget_put(void)
{
	unsigned long flags;
	bool released = false;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	if (vhuge_damon_hot_budget_in_use) {
		vhuge_damon_hot_budget_in_use--;
		released = true;
	}
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	if (released)
		vhuge_stats.damon_budget_released++;
}

static int vhuge_damon_budget_tag(unsigned long hp_index)
{
	return xa_insert(&vhuge_damon_budget_xa, hp_index, xa_mk_value(1),
			 GFP_ATOMIC);
}

static void vhuge_damon_budget_release_hp(unsigned long hp_index)
{
	if (xa_erase(&vhuge_damon_budget_xa, hp_index))
		vhuge_damon_budget_put();
}

static void vhuge_damon_budget_clear(void)
{
	unsigned long index = 0;
	unsigned long flags;
	void *entry;

	xa_lock_irqsave(&vhuge_damon_budget_xa, flags);
	while ((entry = xa_find(&vhuge_damon_budget_xa, &index, ULONG_MAX,
				XA_PRESENT))) {
		__xa_erase(&vhuge_damon_budget_xa, index);
		index++;
	}
	xa_unlock_irqrestore(&vhuge_damon_budget_xa, flags);

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	vhuge_damon_hot_budget_in_use = 0;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
}

static void vhuge_hot_map_clear(struct xarray *xa)
{
	unsigned long index = 0;

	while (xa_find(xa, &index, ULONG_MAX, XA_PRESENT)) {
		xa_erase(xa, index);
		index++;
	}
}

static struct vhuge_monitor_target *vhuge_monitor_find_locked(struct pid *pid)
{
	struct vhuge_monitor_target *target;

	list_for_each_entry(target, &vhuge_monitor_targets, list) {
		if (target->pid == pid)
			return target;
	}
	return NULL;
}

static void vhuge_monitor_target_free(struct vhuge_monitor_target *target)
{
	vhuge_hot_map_clear(&target->hot_pmds);
	xa_destroy(&target->hot_pmds);
	put_pid(target->pid);
	kfree(target);
}

static void vhuge_monitor_clear(void)
{
	struct vhuge_monitor_target *target, *tmp;

	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry_safe(target, tmp, &vhuge_monitor_targets, list) {
		list_del(&target->list);
		vhuge_monitor_target_free(target);
	}
	vhuge_monitor_target_count = 0;
	vhuge_damon_targets_dirty = false;
	mutex_unlock(&vhuge_monitor_lock);
}

static void vhuge_monitor_clear_hot(void)
{
	struct vhuge_monitor_target *target;

	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry(target, &vhuge_monitor_targets, list) {
		vhuge_hot_map_clear(&target->hot_pmds);
	}
	mutex_unlock(&vhuge_monitor_lock);
}

static void vhuge_monitor_reap_dead_locked(void);

static struct pid *vhuge_get_mm_pid(struct mm_struct *mm)
{
	struct task_struct *task = NULL, *thread;
	struct pid *pid = NULL;

	if (!mm)
		return NULL;
	if (!(current->flags & PF_KTHREAD) && current->mm == mm)
		return get_task_pid(current, PIDTYPE_TGID);

#ifdef CONFIG_MEMCG
	rcu_read_lock();
	task = rcu_dereference(mm->owner);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();
	if (task) {
		if (READ_ONCE(task->mm) == mm)
			pid = get_task_pid(task, PIDTYPE_TGID);
		put_task_struct(task);
		if (pid)
			return pid;
	}
#endif

	rcu_read_lock();
	for_each_process_thread(task, thread) {
		if (READ_ONCE(thread->mm) != mm)
			continue;
		pid = get_task_pid(thread, PIDTYPE_TGID);
		break;
	}
	rcu_read_unlock();
	return pid;
}

static void vhuge_monitor_register_mm(struct mm_struct *mm)
{
	struct vhuge_monitor_target *target, *new_target;
	struct pid *pid;
	bool exists;
	bool rebuild = false;

	pid = vhuge_get_mm_pid(mm);
	if (!pid)
		return;
	mutex_lock(&vhuge_monitor_lock);
	exists = vhuge_monitor_find_locked(pid) != NULL;
	mutex_unlock(&vhuge_monitor_lock);
	if (exists) {
		put_pid(pid);
		return;
	}

	new_target = kzalloc(sizeof(*new_target), GFP_KERNEL);
	if (!new_target) {
		put_pid(pid);
		return;
	}
	INIT_LIST_HEAD(&new_target->list);
	new_target->pid = pid;
	xa_init(&new_target->hot_pmds);

	mutex_lock(&vhuge_monitor_lock);
	target = vhuge_monitor_find_locked(pid);
	if (!target && vhuge_monitor_target_count >=
			VHUGE_DAMON_TARGET_MAX_DEF)
		vhuge_monitor_reap_dead_locked();
	if (READ_ONCE(vhuge_enabled) && !target &&
	    vhuge_monitor_target_count <
			VHUGE_DAMON_TARGET_MAX_DEF) {
		list_add_tail(&new_target->list, &vhuge_monitor_targets);
		vhuge_monitor_target_count++;
		vhuge_damon_targets_dirty = true;
		vhuge_stats.damon_targets_registered++;
		rebuild = READ_ONCE(vhuge_damon_running) ||
			(READ_ONCE(vhuge_damon_policy_mode) ==
				VHUGE_DAMON_POLICY_FIXED_BUDGET &&
			 READ_ONCE(vhuge_damon_mode) != VHUGE_DAMON_IDLE);
		new_target = NULL;
	}
	mutex_unlock(&vhuge_monitor_lock);

	if (new_target)
		vhuge_monitor_target_free(new_target);
	if (rebuild)
		mod_delayed_work(system_wq, &vhuge_damon_rebuild_work, 0);
}

static bool vhuge_monitor_addr_hot(struct mm_struct *mm, unsigned long addr)
{
	struct vhuge_monitor_target *target;
	struct pid *pid;
	bool hot = false;

	pid = vhuge_get_mm_pid(mm);
	if (!pid)
		return false;
	mutex_lock(&vhuge_monitor_lock);
	target = vhuge_monitor_find_locked(pid);
	if (target)
		hot = !!xa_load(&target->hot_pmds,
				 addr >> HPAGE_PMD_SHIFT);
	mutex_unlock(&vhuge_monitor_lock);
	put_pid(pid);
	return hot;
}

static bool vhuge_monitor_pid_has_mm(struct pid *pid)
{
	struct task_struct *task;
	bool has_mm;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return false;
	task_lock(task);
	has_mm = task->mm != NULL;
	task_unlock(task);
	put_task_struct(task);
	return has_mm;
}

static void vhuge_monitor_reap_dead_locked(void)
{
	struct vhuge_monitor_target *target, *tmp;

	list_for_each_entry_safe(target, tmp, &vhuge_monitor_targets, list) {
		if (vhuge_monitor_pid_has_mm(target->pid))
			continue;
		list_del(&target->list);
		vhuge_monitor_target_count--;
		vhuge_stats.damon_targets_reaped++;
		vhuge_monitor_target_free(target);
	}
}

static bool vhuge_monitor_has_live_target(void)
{
	struct vhuge_monitor_target *target;
	bool live = false;

	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry(target, &vhuge_monitor_targets, list) {
		if (!vhuge_monitor_pid_has_mm(target->pid))
			continue;
		live = true;
		break;
	}
	if (!live)
		vhuge_damon_targets_dirty = true;
	mutex_unlock(&vhuge_monitor_lock);
	return live;
}

static bool vhuge_owned_hp_lookup(unsigned long hp_index)
{
	unsigned long flags;
	bool owned;

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	owned = !!xa_load(&vhuge_owned_xa, hp_index);
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);
	return owned;
}

static bool vhuge_damon_region_hot(struct damon_region *region)
{
	return region->nr_accesses_bp >=
	       READ_ONCE(vhuge_damon_hot_threshold_bp);
}

static int vhuge_damon_after_aggregation(struct damon_ctx *ctx)
{
	struct damon_target *target;
	struct vhuge_monitor_target *monitor;
	struct damon_region *region;
	ktime_t started = ktime_get();
	unsigned long addr, end;
	u64 elapsed_us;
	u32 limit = READ_ONCE(vhuge_damon_hot_pmd_max);
	u32 hot = 0;
	u32 scout_aggrs;
	bool full = false;
	void *old;

	if (!READ_ONCE(vhuge_damon_enabled) || !READ_ONCE(vhuge_enabled))
		return 0;

	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry(monitor, &vhuge_monitor_targets, list)
		vhuge_hot_map_clear(&monitor->hot_pmds);
	damon_for_each_target(target, ctx) {
		monitor = vhuge_monitor_find_locked(target->pid);
		if (!monitor)
			continue;
		damon_for_each_region(region, target) {
			if (!vhuge_damon_region_hot(region))
				continue;
			addr = ALIGN_DOWN(region->ar.start, HPAGE_PMD_SIZE);
			end = ALIGN(region->ar.end, HPAGE_PMD_SIZE);
			for (; addr < end; addr += HPAGE_PMD_SIZE) {
				old = xa_load(&monitor->hot_pmds,
						      addr >> HPAGE_PMD_SHIFT);
				if (old && xa_to_value(old) - 1 >=
					   region->nr_accesses_bp)
					continue;
				if (!old && hot >= limit) {
					full = true;
					break;
				}
				old = xa_store(&monitor->hot_pmds,
					       addr >> HPAGE_PMD_SHIFT,
					       xa_mk_value(region->nr_accesses_bp + 1),
					       GFP_KERNEL);
				if (xa_is_err(old)) {
					full = true;
					break;
				}
				if (!old)
					hot++;
			}
			if (full)
				break;
		}
		if (full)
			break;
	}
	mutex_unlock(&vhuge_monitor_lock);

	if (READ_ONCE(vhuge_damon_policy_mode) ==
			VHUGE_DAMON_POLICY_FIXED_BUDGET &&
	    READ_ONCE(vhuge_damon_mode) == VHUGE_DAMON_SCOUT &&
	    READ_ONCE(vhuge_damon_running)) {
		scout_aggrs = READ_ONCE(vhuge_damon_fixed_aggr_count) + 1;
		WRITE_ONCE(vhuge_damon_fixed_aggr_count, scout_aggrs);
		if (scout_aggrs >=
		    READ_ONCE(vhuge_damon_fixed_scout_aggrs)) {
			WRITE_ONCE(vhuge_damon_mode, VHUGE_DAMON_PRESSURE);
			vhuge_stats.damon_fixed_activated++;
			mod_delayed_work(vhuge_damon_wq,
					 &vhuge_damon_classify_work, 0);
			pr_info("vhuge: fixed-budget placement active budget=%u after %u DAMON aggregations\n",
				READ_ONCE(vhuge_damon_hot_budget_pages),
				scout_aggrs);
		}
	}

	elapsed_us = ktime_us_delta(ktime_get(), started);
	update_timing(elapsed_us, &vhuge_stats.damon_last_update_us,
		      &vhuge_stats.damon_max_update_us);
	WRITE_ONCE(vhuge_stats.damon_hot_pmd_ranges, hot);
	vhuge_stats.damon_aggregations++;
	if (full)
		vhuge_stats.damon_hot_table_full++;
	return 0;
}
#endif

static void vhuge_free_entry(struct vhuge_pool_entry *entry)
{
	if (entry->allocated)
		__free_pages(entry->page, HPAGE_PMD_ORDER);
	else
		vhuge_free_prepared_page(entry->page, HPAGE_PMD_ORDER);
	kfree(entry);
}

static void vhuge_quarantine_entry(struct vhuge_pool_entry *entry)
{
	unsigned long flags;

	entry->validation_slot = 0;
	spin_lock_irqsave(&vhuge_quarantine_lock, flags);
	list_add_tail(&entry->list, &vhuge_quarantine);
	vhuge_stats.validation_quarantined++;
	spin_unlock_irqrestore(&vhuge_quarantine_lock, flags);
	pr_warn_ratelimited("vhuge: quarantined validation page pfn=%lu after failed free-state preparation\n",
			    entry->pfn);
}

static void vhuge_return_entry_to_pool(struct vhuge_pool_entry *entry,
				       bool validation)
{
	unsigned long flags;
	bool keep;

	entry->validation_slot = 0;
	if (entry->allocated) {
		if (!vhuge_prepare_free_page(entry->page, HPAGE_PMD_ORDER)) {
			vhuge_quarantine_entry(entry);
			return;
		}
		entry->allocated = false;
	}

	spin_lock_irqsave(&vhuge_pool_lock, flags);
	keep = READ_ONCE(vhuge_enabled) &&
	       vhuge_pool_count < READ_ONCE(vhuge_pool_max);
	if (keep) {
		list_add_tail(&entry->list, &vhuge_pool);
		vhuge_pool_count++;
		if (validation)
			vhuge_stats.validation_returned++;
	}
	spin_unlock_irqrestore(&vhuge_pool_lock, flags);

	if (!keep) {
		if (validation)
			vhuge_stats.validation_released++;
		else
			vhuge_stats.released++;
		vhuge_free_entry(entry);
	}
}

static void vhuge_return_validation_entries(struct vhuge_file_ctx *ctx)
{
	struct vhuge_pool_entry *entry, *tmp;
	LIST_HEAD(local);
	u32 count;

	mutex_lock(&ctx->validating_lock);
	count = ctx->validating_count;
	list_splice_init(&ctx->validating, &local);
	atomic_sub(ctx->validating_count, &vhuge_validation_in_progress);
	ctx->validating_count = 0;
	mutex_unlock(&ctx->validating_lock);
	if (count)
		pr_info("vhuge: returning %u outstanding validation pages for pid=%d comm=%s\n",
			count, task_pid_nr(current), current->comm);
	list_for_each_entry_safe(entry, tmp, &local, list) {
		list_del(&entry->list);
		vhuge_return_entry_to_pool(entry, true);
	}
}

static void vhuge_ctx_put(struct vhuge_file_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs))
		kfree(ctx);
}

static void vhuge_unregister_daemon_ctx(struct vhuge_file_ctx *ctx)
{
	if (!ctx->daemon_registered)
		return;
	WRITE_ONCE(vhuge_daemon_mm, NULL);
	mmdrop(ctx->mm);
	ctx->daemon_registered = false;
	atomic_set(&vhuge_daemon_open, 0);
}

static struct vhuge_mm_owner *vhuge_find_owner_locked(struct mm_struct *mm)
{
	struct vhuge_mm_owner *owner;

	list_for_each_entry(owner, &vhuge_owner_list, list) {
		if (owner->mm == mm)
			return owner;
	}
	return NULL;
}

static void vhuge_drop_owner_locked(struct vhuge_mm_owner *owner)
{
	list_del(&owner->list);
	kfree(owner);
}

static void vhuge_dec_owner_locked(struct vhuge_owned_hp *owned)
{
	struct vhuge_mm_owner *owner = owned->owner;

	if (owner->real_hps)
		owner->real_hps--;
	if (owned->source == VHUGE_HP_SRC_THP_FAULT && owner->thp_fault_hps)
		owner->thp_fault_hps--;
	else if (owned->source == VHUGE_HP_SRC_KHUGEPAGED &&
		 owner->khugepaged_hps)
		owner->khugepaged_hps--;
	else if (owned->source == VHUGE_HP_SRC_DAMON_PROBE &&
		 owner->damon_probe_hps)
		owner->damon_probe_hps--;
	else if (owned->source == VHUGE_HP_SRC_MIGRATED &&
		 owner->migrated_hps)
		owner->migrated_hps--;
	if (vhuge_stats.owned_current)
		vhuge_stats.owned_current--;
	if (!owner->real_hps)
		vhuge_drop_owner_locked(owner);
}

static void vhuge_fill_owner_identity(struct vhuge_mm_owner *owner,
				      struct mm_struct *mm)
{
	struct task_struct *task = NULL, *thread = NULL;
	bool found = false;

	if (!(current->flags & PF_KTHREAD) && current->mm == mm) {
		owner->tgid = task_tgid_nr(current);
		get_task_comm(owner->comm, current);
		return;
	}

#ifdef CONFIG_MEMCG
	rcu_read_lock();
	task = rcu_dereference(mm->owner);
	if (task) {
		owner->tgid = task_tgid_nr(task);
		get_task_comm(owner->comm, task);
	}
	rcu_read_unlock();
#endif

	if (task)
		return;

	rcu_read_lock();
	for_each_process_thread(task, thread) {
		if (READ_ONCE(thread->mm) != mm)
			continue;
		owner->tgid = task_tgid_nr(thread);
		get_task_comm(owner->comm, thread);
		found = true;
		break;
	}
	rcu_read_unlock();

	if (!found) {
		owner->tgid = 0;
		strscpy(owner->comm, "kernel-workers", sizeof(owner->comm));
	}
}

static void vhuge_remove_owned_hp(unsigned long hp_index, bool split)
{
	struct vhuge_owned_hp *owned;
	unsigned long flags;

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	owned = xa_erase(&vhuge_owned_xa, hp_index);
	if (!owned) {
		spin_unlock_irqrestore(&vhuge_owner_lock, flags);
		return;
	}

	if (split)
		vhuge_stats.ownership_splits++;
	vhuge_dec_owner_locked(owned);
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);

	kfree(owned);
}

static int vhuge_add_owned_hp(struct folio *folio,
			      enum vhuge_hp_source source,
			      struct mm_struct *mm)
{
	struct vhuge_owned_hp *owned, *old_owned;
	struct vhuge_mm_owner *owner, *new_owner = NULL;
	unsigned long hp_index = pfn_to_hp_index(folio_pfn(folio));
	unsigned long flags;
	int ret = 0;

	owned = kzalloc(sizeof(*owned), GFP_KERNEL);
	if (!owned)
		return -ENOMEM;
	owned->source = source;

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	owner = vhuge_find_owner_locked(mm);
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);

	if (!owner) {
		new_owner = kzalloc(sizeof(*new_owner), GFP_KERNEL);
		if (!new_owner) {
			kfree(owned);
			return -ENOMEM;
		}
		new_owner->mm = mm;
		vhuge_fill_owner_identity(new_owner, mm);
	}

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	owner = vhuge_find_owner_locked(mm);
	if (!owner && new_owner) {
		owner = new_owner;
		new_owner = NULL;
		list_add_tail(&owner->list, &vhuge_owner_list);
	}
	if (!owner) {
		ret = -ENOENT;
		vhuge_stats.ownership_lost++;
		goto out_unlock;
	}
	owned->owner = owner;
	old_owned = xa_load(&vhuge_owned_xa, hp_index);
	if (old_owned) {
		ret = -EEXIST;
		vhuge_stats.ownership_lost++;
	} else {
		old_owned = xa_store(&vhuge_owned_xa, hp_index, owned,
				     GFP_ATOMIC);
		if (xa_is_err(old_owned)) {
			ret = xa_err(old_owned);
		} else {
			owner->real_hps++;
			if (source == VHUGE_HP_SRC_THP_FAULT)
				owner->thp_fault_hps++;
			else if (source == VHUGE_HP_SRC_KHUGEPAGED)
				owner->khugepaged_hps++;
			else if (source == VHUGE_HP_SRC_DAMON_PROBE)
				owner->damon_probe_hps++;
			else if (source == VHUGE_HP_SRC_MIGRATED)
				owner->migrated_hps++;
			vhuge_stats.owned_current++;
		}
	}
out_unlock:
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);

	if (new_owner)
		kfree(new_owner);
	if (ret)
		kfree(owned);
	return ret;
}

#ifdef CONFIG_VHUGE_DAMON
static int cmp_u64(const void *a, const void *b)
{
	const u64 va = *(const u64 *)a;
	const u64 vb = *(const u64 *)b;

	if (va < vb)
		return -1;
	if (va > vb)
		return 1;
	return 0;
}

static struct vhuge_probe_record *vhuge_probe_record_alloc(
		enum vhuge_probe_state state)
{
	struct vhuge_probe_record *record;

	record = kzalloc(sizeof(*record), GFP_KERNEL);
	if (record)
		record->state = state;
	return record;
}

static bool vhuge_probe_fake_recent(unsigned long hp_index)
{
	struct vhuge_probe_record *record;
	bool fake = false;

	mutex_lock(&vhuge_probe_lock);
	record = xa_load(&vhuge_probe_xa, hp_index);
	if (record && record->state == VHUGE_PROBE_FAKE) {
		if (time_before(jiffies, record->expires)) {
			fake = true;
		} else {
			xa_erase(&vhuge_probe_xa, hp_index);
			kfree(record);
		}
	}
	mutex_unlock(&vhuge_probe_lock);
	return fake;
}

static int vhuge_probe_mark_inflight(unsigned long hp_index)
{
	struct vhuge_probe_record *record, *old;
	int ret = 0;

	record = vhuge_probe_record_alloc(VHUGE_PROBE_INFLIGHT);
	if (!record)
		return -ENOMEM;

	mutex_lock(&vhuge_probe_lock);
	old = xa_load(&vhuge_probe_xa, hp_index);
	if (old) {
		if (old->state == VHUGE_PROBE_FAKE &&
		    !time_before(jiffies, old->expires)) {
			xa_erase(&vhuge_probe_xa, hp_index);
			kfree(old);
		} else {
			ret = -EEXIST;
			goto out_unlock;
		}
	}
	old = xa_store(&vhuge_probe_xa, hp_index, record, GFP_KERNEL);
	if (xa_is_err(old)) {
		ret = xa_err(old);
		goto out_unlock;
	}
	record = NULL;
	atomic_inc(&vhuge_probe_inflight);

out_unlock:
	mutex_unlock(&vhuge_probe_lock);
	kfree(record);
	return ret;
}

static void vhuge_probe_finish(unsigned long hp_index, bool fake)
{
	struct vhuge_probe_record *record, *old;

	mutex_lock(&vhuge_probe_lock);
	old = xa_erase(&vhuge_probe_xa, hp_index);
	if (old && old->state == VHUGE_PROBE_INFLIGHT)
		atomic_dec_if_positive(&vhuge_probe_inflight);
	kfree(old);

	if (fake) {
		record = vhuge_probe_record_alloc(VHUGE_PROBE_FAKE);
		if (record) {
			record->expires = jiffies +
				READ_ONCE(vhuge_damon_probe_cooldown_sec) * HZ;
			old = xa_store(&vhuge_probe_xa, hp_index, record,
				       GFP_KERNEL);
			if (xa_is_err(old))
				kfree(record);
			else
				kfree(old);
		}
	}
	mutex_unlock(&vhuge_probe_lock);
}

static void vhuge_probe_clear(void)
{
	struct vhuge_probe_record *record;
	unsigned long index = 0;

	mutex_lock(&vhuge_probe_lock);
	while ((record = xa_find(&vhuge_probe_xa, &index, ULONG_MAX,
				 XA_PRESENT))) {
		xa_erase(&vhuge_probe_xa, index);
		if (record->state == VHUGE_PROBE_INFLIGHT)
			atomic_dec_if_positive(&vhuge_probe_inflight);
		kfree(record);
		index++;
	}
	mutex_unlock(&vhuge_probe_lock);
}

static void vhuge_probe_clear_fake(unsigned long hp_index)
{
	struct vhuge_probe_record *record;

	mutex_lock(&vhuge_probe_lock);
	record = xa_load(&vhuge_probe_xa, hp_index);
	if (record && record->state == VHUGE_PROBE_FAKE) {
		xa_erase(&vhuge_probe_xa, hp_index);
		kfree(record);
	}
	mutex_unlock(&vhuge_probe_lock);
}

static int vhuge_revalidate_pmd(struct mm_struct *mm, struct folio *expected,
				unsigned long addr)
{
	struct vm_area_struct *vma;
	spinlock_t *ptl;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmdp, pmd;
	int ret = -EAGAIN;

	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start ||
	    addr + HPAGE_PMD_SIZE > vma->vm_end)
		return -EFAULT;
	if (!vhuge_damon_vma_allowed(vma))
		return -EACCES;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return -EAGAIN;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return -EAGAIN;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return -EAGAIN;

	pmdp = pmd_offset(pud, addr);
	ptl = pmd_trans_huge_lock(pmdp, vma);
	if (!ptl)
		return -EAGAIN;
	pmd = *pmdp;
	if (!pmd_present(pmd) || !pmd_trans_huge(pmd) ||
	    is_huge_zero_pmd(pmd))
		goto out_unlock;
	/* A PMD mapping of the expected folio implies its order and index. */
	if (pmd_folio(pmd) != expected)
		goto out_unlock;
	if (folio_entire_mapcount(expected) != 1) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = 0;

out_unlock:
	spin_unlock(ptl);
	return ret;
}

static int vhuge_probe_user_read(u8 __user *ptr)
{
	u8 value;
	int ret = 0;

	if (!user_access_begin(ptr, sizeof(*ptr)))
		return -EFAULT;
	pagefault_disable();
	unsafe_get_user(value, ptr, fault);
	(void)value;
	goto out;
fault:
	ret = -EFAULT;
out:
	pagefault_enable();
	user_access_end();
	return ret;
}

static int vhuge_probe_user_timed_read(u8 __user *ptr, u64 *cycles)
{
	u8 value;
	u64 t0, t1;
	int ret = 0;

	if (!user_access_begin(ptr, sizeof(*ptr)))
		return -EFAULT;
	pagefault_disable();
	t0 = rdtsc_ordered();
	unsafe_get_user(value, ptr, fault);
	(void)value;
	t1 = rdtsc_ordered();
	goto out;
fault:
	ret = -EFAULT;
out:
	pagefault_enable();
	user_access_end();
	if (!ret)
		*cycles = t1 - t0;
	return ret;
}

static int vhuge_probe_user_once(struct mm_struct *mm, unsigned long addr,
				 u64 *cycles)
{
	u8 __user *middle = (u8 __user *)(addr + (HPAGE_PMD_SIZE / 2));
	int ret;

	/* Match the mm-scoped invalidation used by the validated userspace probe. */
	migrate_disable();
	ret = vhuge_probe_user_read((u8 __user *)addr);
	if (ret)
		goto out;
	__rmb();
	flush_tlb_mm_range(mm, addr, addr + PAGE_SIZE, PAGE_SHIFT, false);
	__mb();
	ret = vhuge_probe_user_read(middle);
	if (ret)
		goto out;
	__mb();
	__rmb();
	ret = vhuge_probe_user_timed_read((u8 __user *)addr, cycles);
	/*
	 * With in-kernel calibration, report the latency above a TLB hit
	 * taken right after, so a shift of the baseline cancels out.
	 */
	if (!ret && vhuge_damon_probe_self_calibrate) {
		u64 hit;

		ret = vhuge_probe_user_timed_read((u8 __user *)addr, &hit);
		*cycles = *cycles > hit ? *cycles - hit : 0;
	}
out:
	migrate_enable();
	return ret;
}

static int vhuge_probe_user_pmd(struct mm_struct *mm,
				unsigned long addr, u64 *median)
{
	u64 samples[VHUGE_DAMON_PROBE_SAMPLES_MAX];
	u32 nr_samples = READ_ONCE(vhuge_damon_probe_samples);
	u32 i;
	int ret;

	nr_samples = clamp(nr_samples, 1U, VHUGE_DAMON_PROBE_SAMPLES_MAX);
	for (i = 0; i < nr_samples; i++) {
		ret = vhuge_probe_user_once(mm, addr, &samples[i]);
		if (ret)
			return ret;
	}
	sort(samples, nr_samples, sizeof(samples[0]), cmp_u64, NULL);
	*median = samples[nr_samples / 2];
	return 0;
}

/*
 * Same calibration as vhuge-poold, measured with the kernel's own timed read.
 * The miss is taken the way a fake huge page shows up in the probe: the TLB
 * entry is gone but the paging-structure caches are warm, here warmed by an
 * access to the neighboring 2 MiB range.
 */
static void vhuge_probe_calibrate(struct mm_struct *mm, unsigned long addr)
{
	u32 n = VHUGE_DAMON_PROBE_CALIB_SAMPLES, i;
	u64 *miss = kvmalloc_array(2 * n, sizeof(*miss), GFP_KERNEL);
	u64 *hit = miss + n;
	u8 __user *ptr = (u8 __user *)addr;
	u8 __user *near = ptr + HPAGE_PMD_SIZE;

	if (!miss)
		return;
	migrate_disable();
	if (vhuge_probe_user_read(near))
		near = ptr - HPAGE_PMD_SIZE;
	for (i = 0; i < n; i++) {
		vhuge_probe_user_read(ptr);
		flush_tlb_mm_range(mm, addr, addr + PAGE_SIZE, PAGE_SHIFT, false);
		if (vhuge_probe_user_read(near) ||
		    vhuge_probe_user_timed_read(ptr, &miss[i]))
			goto out;
	}
	for (i = 0; i < n; i++) {
		vhuge_probe_user_read(ptr);
		if (vhuge_probe_user_timed_read(ptr, &hit[i]))
			goto out;
	}
	sort(miss, n, sizeof(*miss), cmp_u64, NULL);
	sort(hit, n, sizeof(*hit), cmp_u64, NULL);
	vhuge_damon_probe_calib_miss = miss[n / 2];
	vhuge_damon_probe_calib_hit = hit[n / 2];
	if (miss[n / 2] > hit[n / 2]) {
		/* (2 * hit + 3 * miss) / 5, as a distance from the hit. */
		WRITE_ONCE(vhuge_damon_probe_threshold_cycles,
			   3 * (miss[n / 2] - hit[n / 2]) / 5);
		vhuge_damon_probe_calibrated = true;
	}
out:
	migrate_enable();
	kvfree(miss);
}

static void vhuge_probe_workfn(struct kthread_work *work)
{
	struct vhuge_damon_candidate *cand =
		container_of(work, struct vhuge_damon_candidate, work);
	ktime_t started = ktime_get();
	u32 threshold;
	u64 elapsed_us, median = 0;
	bool fake = false;
	int ret = -EAGAIN;

	vhuge_stats.damon_probe_attempted++;
	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_running))
		goto out_finish;

	kthread_use_mm(cand->mm);
	mmap_read_lock(cand->mm);
	ret = vhuge_revalidate_pmd(cand->mm, cand->folio, cand->addr);
	if (!ret && vhuge_damon_probe_self_calibrate &&
	    !vhuge_damon_probe_calibrated)
		vhuge_probe_calibrate(cand->mm, cand->addr);
	threshold = READ_ONCE(vhuge_damon_probe_threshold_cycles);
	if (!ret)
		ret = vhuge_probe_user_pmd(cand->mm, cand->addr, &median);
	if (!ret)
		ret = vhuge_revalidate_pmd(cand->mm, cand->folio, cand->addr);
	mmap_read_unlock(cand->mm);
	kthread_unuse_mm(cand->mm);

	if (ret)
		goto out_finish;

	update_timing(median, &vhuge_stats.damon_probe_last_median_cycles,
		      &vhuge_stats.damon_probe_max_median_cycles);
	fake = median >= threshold;
	if (fake) {
		vhuge_stats.damon_probe_fake++;
	} else if (!vhuge_owned_hp_lookup(cand->hp_index)) {
		mutex_lock(&vhuge_enable_lock);
		if (!READ_ONCE(vhuge_enabled)) {
			ret = -EAGAIN;
		} else if (!vhuge_owned_hp_lookup(cand->hp_index)) {
			ret = vhuge_add_owned_hp(cand->folio,
						 VHUGE_HP_SRC_DAMON_PROBE,
						 cand->mm);
			if (!ret)
				vhuge_stats.damon_probe_real++;
		}
		mutex_unlock(&vhuge_enable_lock);
		if (ret)
			goto out_finish;
	}

out_finish:
	if (ret) {
		vhuge_stats.damon_probe_failed++;
		if (ret == -EFAULT)
			vhuge_stats.damon_probe_faulted++;
	}
	vhuge_probe_finish(cand->hp_index, fake && !ret);
	mmput(cand->mm);
	folio_put(cand->folio);
	kfree(cand);
	elapsed_us = ktime_us_delta(ktime_get(), started);
	update_timing(elapsed_us, &vhuge_stats.damon_probe_last_us,
		      &vhuge_stats.damon_probe_max_us);
}

static void vhuge_probe_queue(struct mm_struct *mm, struct folio *folio,
			      unsigned long addr, unsigned long hp_index,
			      struct vhuge_damon_scan_result *result)
{
	struct vhuge_damon_candidate *cand;
	int ret;

	if (!READ_ONCE(vhuge_damon_probe_max_per_scan))
		goto out_drop;
	if (result->probes_queued >= READ_ONCE(vhuge_damon_probe_max_per_scan)) {
		result->probe_budget_hit = true;
		goto out_drop;
	}

	ret = vhuge_probe_mark_inflight(hp_index);
	if (ret == -EEXIST) {
		vhuge_stats.damon_probe_inflight_skip++;
		goto out_drop;
	}
	if (ret)
		goto out_drop;

	cand = kzalloc(sizeof(*cand), GFP_KERNEL);
	if (!cand) {
		vhuge_probe_finish(hp_index, false);
		goto out_drop;
	}
	kthread_init_work(&cand->work, vhuge_probe_workfn);
	cand->mm = mm;
	cand->folio = folio;
	cand->addr = addr & HPAGE_PMD_MASK;
	cand->hp_index = hp_index;

	if (!kthread_queue_work(vhuge_probe_worker, &cand->work)) {
		vhuge_probe_finish(hp_index, false);
		kfree(cand);
		goto out_drop;
	}
	result->probes_queued++;
	vhuge_stats.damon_probe_queued++;
	return;

out_drop:
	mmput(mm);
	folio_put(folio);
}

static int vhuge_migrate_mark_inflight(unsigned long hp_index)
{
	void *old;
	int ret = 0;

	mutex_lock(&vhuge_migrate_lock);
	old = xa_load(&vhuge_migrate_xa, hp_index);
	if (old) {
		ret = -EEXIST;
		goto out_unlock;
	}
	old = xa_store(&vhuge_migrate_xa, hp_index, xa_mk_value(1),
		       GFP_KERNEL);
	if (xa_is_err(old)) {
		ret = xa_err(old);
		goto out_unlock;
	}
	atomic_inc(&vhuge_migrate_inflight);

out_unlock:
	mutex_unlock(&vhuge_migrate_lock);
	return ret;
}

static void vhuge_migrate_finish(unsigned long hp_index)
{
	mutex_lock(&vhuge_migrate_lock);
	if (xa_erase(&vhuge_migrate_xa, hp_index))
		atomic_dec_if_positive(&vhuge_migrate_inflight);
	mutex_unlock(&vhuge_migrate_lock);
}

static void vhuge_migrate_clear(void)
{
	void *entry;
	unsigned long index = 0;

	mutex_lock(&vhuge_migrate_lock);
	while ((entry = xa_find(&vhuge_migrate_xa, &index, ULONG_MAX,
				XA_PRESENT))) {
		xa_erase(&vhuge_migrate_xa, index);
		atomic_dec_if_positive(&vhuge_migrate_inflight);
		index++;
	}
	mutex_unlock(&vhuge_migrate_lock);
}

static struct folio *vhuge_migrate_get_target(struct folio *src,
					      unsigned long private)
{
	struct vhuge_migrate_target *target =
		(struct vhuge_migrate_target *)private;

	if (target->handed || !target->folio)
		return NULL;
	if (folio_order(src) != HPAGE_PMD_ORDER ||
	    !folio_test_pmd_mappable(src))
		return NULL;
	target->handed = true;
	return target->folio;
}

static void vhuge_migrate_put_target(struct folio *folio, unsigned long private)
{
	struct vhuge_migrate_target *target =
		(struct vhuge_migrate_target *)private;

	if (folio == target->folio)
		target->handed = false;
}

static void vhuge_migrate_workfn(struct kthread_work *work)
{
	struct vhuge_damon_candidate *cand =
		container_of(work, struct vhuge_damon_candidate, work);
	struct vhuge_migrate_target target = {};
	LIST_HEAD(source);
	ktime_t total_start = ktime_get();
	ktime_t start;
	u64 total_us, isolate_us = 0, pool_us = 0;
	u64 migrate_us = 0, account_us = 0;
	bool budget_reserved = false;
	bool budget_tagged = false;
	bool source_ref = true;
	bool migrated_ok = false;
	int ret = -EAGAIN;

	vhuge_stats.damon_migrate_attempted++;
	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_running) ||
	    !thp_migration_supported())
		goto out_fail;

	mmap_read_lock(cand->mm);
	ret = vhuge_revalidate_pmd(cand->mm, cand->folio, cand->addr);
	mmap_read_unlock(cand->mm);
	if (ret) {
		vhuge_stats.damon_migrate_revalidate_failed++;
		goto out_fail;
	}
	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_running))
		goto out_fail;

	start = ktime_get();
	if (READ_ONCE(vhuge_damon_policy_mode) ==
			VHUGE_DAMON_POLICY_FIXED_BUDGET) {
		if (!vhuge_damon_budget_try_reserve(false))
			goto out_fail;
		budget_reserved = true;
	}
	target.entry = vhuge_take_pool_entry(NUMA_NO_NODE, false);
	pool_us = ktime_us_delta(ktime_get(), start);
	if (!target.entry) {
		vhuge_stats.damon_migrate_no_pool++;
		goto out_fail;
	}
	if (budget_reserved) {
		ret = vhuge_damon_budget_tag(
			pfn_to_hp_index(target.entry->pfn));
		if (ret) {
			vhuge_return_entry_to_pool(target.entry, false);
			target.entry = NULL;
			goto out_fail;
		}
		budget_tagged = true;
	}
	vhuge_prepare_allocated_page(target.entry->page, HPAGE_PMD_ORDER,
				     GFP_TRANSHUGE);
	target.entry->allocated = true;
	target.folio = page_rmappable_folio(target.entry->page);
	folio_get(target.folio);

	start = ktime_get();
	if (!isolate_folio_to_list(cand->folio, &source)) {
		vhuge_stats.damon_migrate_isolate_failed++;
		ret = -EBUSY;
		goto out_return_target;
	}
	folio_put(cand->folio);
	source_ref = false;
	isolate_us = ktime_us_delta(ktime_get(), start);

	start = ktime_get();
	ret = migrate_pages(&source, vhuge_migrate_get_target,
			    vhuge_migrate_put_target, (unsigned long)&target,
			    MIGRATE_SYNC_LIGHT, MR_DAMON, NULL);
	migrate_us = ktime_us_delta(ktime_get(), start);
	if (ret || !list_empty(&source) || !target.handed) {
		if (!list_empty(&source))
			putback_movable_pages(&source);
		vhuge_stats.damon_migrate_pages_failed++;
		goto out_return_target;
	}
	migrated_ok = true;
	vhuge_stats.damon_migrate_succeeded++;
	vhuge_stats.consumed++;
	if (xa_err(xa_store(&vhuge_consumed_xa,
			    pfn_to_hp_index(target.entry->pfn),
			    xa_mk_value(1), GFP_KERNEL)))
		vhuge_stats.ownership_lost++;
	vhuge_probe_clear_fake(cand->hp_index);

	start = ktime_get();
	ret = vhuge_add_owned_hp(target.folio, VHUGE_HP_SRC_MIGRATED,
				 cand->mm);
	if (ret)
		vhuge_stats.damon_migrate_account_failed++;
	account_us = ktime_us_delta(ktime_get(), start);

out_return_target:
	if (!migrated_ok && budget_tagged) {
		vhuge_damon_budget_release_hp(
			pfn_to_hp_index(target.entry->pfn));
		budget_tagged = false;
		budget_reserved = false;
	}
	if (target.entry && migrated_ok) {
		folio_put(target.folio);
		kfree(target.entry);
	} else if (target.entry) {
		if (!target.handed)
			folio_put(target.folio);
		vhuge_return_entry_to_pool(target.entry, false);
	}

out_fail:
	if (!migrated_ok && budget_reserved)
		vhuge_damon_budget_put();
	if (!migrated_ok)
		vhuge_stats.damon_migrate_failed++;
	if (source_ref)
		folio_put(cand->folio);
	mmput(cand->mm);
	vhuge_migrate_finish(cand->hp_index);
	kfree(cand);

	total_us = ktime_us_delta(ktime_get(), total_start);
	update_timing(total_us, &vhuge_stats.damon_migrate_last_total_us,
		      &vhuge_stats.damon_migrate_max_total_us);
	update_timing(isolate_us, &vhuge_stats.damon_migrate_last_isolate_us,
		      &vhuge_stats.damon_migrate_max_isolate_us);
	update_timing(pool_us, &vhuge_stats.damon_migrate_last_pool_us,
		      &vhuge_stats.damon_migrate_max_pool_us);
	update_timing(migrate_us, &vhuge_stats.damon_migrate_last_migrate_us,
		      &vhuge_stats.damon_migrate_max_migrate_us);
	update_timing(account_us, &vhuge_stats.damon_migrate_last_account_us,
		      &vhuge_stats.damon_migrate_max_account_us);
}

static void vhuge_migrate_queue(struct mm_struct *mm, struct folio *folio,
				unsigned long addr, unsigned long hp_index,
				struct vhuge_damon_scan_result *result)
{
	struct vhuge_damon_candidate *cand;
	int ret;

	if (!READ_ONCE(vhuge_damon_migrate_max_per_scan))
		goto out_drop;
	if (!READ_ONCE(vhuge_pool_count)) {
		vhuge_stats.damon_migrate_no_pool++;
		goto out_drop;
	}
	if (result->migrations_queued >=
	    READ_ONCE(vhuge_damon_migrate_max_per_scan)) {
		result->migrate_limit_hit = true;
		goto out_drop;
	}

	ret = vhuge_migrate_mark_inflight(hp_index);
	if (ret)
		goto out_drop;

	cand = kzalloc(sizeof(*cand), GFP_KERNEL);
	if (!cand) {
		vhuge_migrate_finish(hp_index);
		goto out_drop;
	}
	kthread_init_work(&cand->work, vhuge_migrate_workfn);
	cand->mm = mm;
	cand->folio = folio;
	cand->addr = addr & HPAGE_PMD_MASK;
	cand->hp_index = hp_index;

	if (!kthread_queue_work(vhuge_migrate_worker, &cand->work)) {
		vhuge_migrate_finish(hp_index);
		kfree(cand);
		goto out_drop;
	}
	result->migrations_queued++;
	vhuge_stats.damon_migrate_queued++;
	return;

out_drop:
	mmput(mm);
	folio_put(folio);
}

static int vhuge_probe_worker_start(void)
{
	vhuge_probe_worker = kthread_create_worker(0, "vhuge_probe");
	if (IS_ERR(vhuge_probe_worker)) {
		int ret = PTR_ERR(vhuge_probe_worker);

		vhuge_probe_worker = NULL;
		return ret;
	}
	return 0;
}

static void vhuge_probe_worker_stop(void)
{
	kthread_destroy_worker(vhuge_probe_worker);
	vhuge_probe_worker = NULL;
	vhuge_probe_clear();
}

static int vhuge_migrate_worker_start(void)
{
	vhuge_migrate_worker = kthread_create_worker(0, "vhuge_migrate");
	if (IS_ERR(vhuge_migrate_worker)) {
		int ret = PTR_ERR(vhuge_migrate_worker);

		vhuge_migrate_worker = NULL;
		return ret;
	}
	return 0;
}

static void vhuge_migrate_worker_stop(void)
{
	kthread_destroy_worker(vhuge_migrate_worker);
	vhuge_migrate_worker = NULL;
	vhuge_migrate_clear();
}
#endif

static void vhuge_drain_pool(void)
{
	LIST_HEAD(local);
	struct vhuge_pool_entry *entry, *tmp;
	unsigned long flags;
	u32 count;

	spin_lock_irqsave(&vhuge_pool_lock, flags);
	count = vhuge_pool_count;
	list_splice_init(&vhuge_pool, &local);
	vhuge_pool_count = 0;
	spin_unlock_irqrestore(&vhuge_pool_lock, flags);

	if (count)
		pr_info("vhuge: draining %u pooled pages\n", count);
	list_for_each_entry_safe(entry, tmp, &local, list) {
		list_del(&entry->list);
		vhuge_stats.released++;
		vhuge_free_entry(entry);
	}
}

static void vhuge_clear_marks(void)
{
	unsigned long index = 0;
	void *entry;
	unsigned long flags;

	xa_lock_irqsave(&vhuge_capture_xa, flags);
	while ((entry = xa_find(&vhuge_capture_xa, &index, ULONG_MAX,
				XA_PRESENT))) {
		__xa_erase(&vhuge_capture_xa, index);
		index++;
	}
	xa_unlock_irqrestore(&vhuge_capture_xa, flags);
}

static void vhuge_clear_consumed(void)
{
	unsigned long index = 0;
	void *entry;
	unsigned long flags;

	xa_lock_irqsave(&vhuge_consumed_xa, flags);
	while ((entry = xa_find(&vhuge_consumed_xa, &index, ULONG_MAX,
				XA_PRESENT))) {
		__xa_erase(&vhuge_consumed_xa, index);
		index++;
	}
	xa_unlock_irqrestore(&vhuge_consumed_xa, flags);
}

static void vhuge_clear_ownership(void)
{
	struct vhuge_mm_owner *owner, *owner_tmp;
	struct vhuge_owned_hp *owned;
	unsigned long index = 0;
	unsigned long flags;

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	while ((owned = xa_find(&vhuge_owned_xa, &index, ULONG_MAX,
				XA_PRESENT))) {
		xa_erase(&vhuge_owned_xa, index);
		kfree(owned);
		index++;
	}
	list_for_each_entry_safe(owner, owner_tmp, &vhuge_owner_list, list)
		vhuge_drop_owner_locked(owner);
	vhuge_stats.owned_current = 0;
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);
}

static bool vhuge_is_daemon_mm(struct mm_struct *mm)
{
	return mm && READ_ONCE(vhuge_daemon_mm) == mm;
}

static bool vhuge_is_userspace_tool_task(void)
{
	return !strcmp(current->comm, VHUGE_TOOL_COMM);
}

static struct vhuge_pool_entry *vhuge_take_pool_entry(int nid,
						      bool count_empty)
{
	struct vhuge_pool_entry *entry, *chosen = NULL, *fallback = NULL;
	unsigned long flags;
	bool allow_fallback;

	allow_fallback = nid != NUMA_NO_NODE;
	spin_lock_irqsave(&vhuge_pool_lock, flags);
	list_for_each_entry(entry, &vhuge_pool, list) {
		if (nid == NUMA_NO_NODE || entry->nid == nid) {
			chosen = entry;
			break;
		}
		if (allow_fallback && !fallback)
			fallback = entry;
	}
	if (!chosen && count_empty && nid != NUMA_NO_NODE)
		vhuge_stats.alloc_local_miss++;
	if (!chosen && allow_fallback)
		chosen = fallback;
	if (!chosen) {
		if (count_empty)
			vhuge_stats.alloc_empty++;
		spin_unlock_irqrestore(&vhuge_pool_lock, flags);
		return NULL;
	}
	if (count_empty && nid != NUMA_NO_NODE) {
		if (chosen->nid == nid)
			vhuge_stats.alloc_local++;
		else
			vhuge_stats.alloc_remote++;
	}
	list_del(&chosen->list);
	vhuge_pool_count--;
	spin_unlock_irqrestore(&vhuge_pool_lock, flags);
	return chosen;
}

struct folio *vhuge_alloc_real_folio(struct mm_struct *mm, unsigned long addr,
				     gfp_t gfp, unsigned int order, int nid)
{
	struct vhuge_pool_entry *entry;
	struct folio *folio;
	unsigned long hp_index;
	int ret;
#ifdef CONFIG_VHUGE_DAMON
	enum vhuge_damon_mode mode;
	enum vhuge_damon_policy_mode policy_mode;
	bool hot;
	bool budget_reserved = false;
	bool budget_tagged = false;
#endif

	if (!READ_ONCE(vhuge_enabled) || order != HPAGE_PMD_ORDER)
		return NULL;
	if (vhuge_is_daemon_mm(mm)) {
		vhuge_stats.daemon_pool_bypass++;
		return NULL;
	}
	if (vhuge_is_userspace_tool_task()) {
		vhuge_stats.tool_pool_bypass++;
		return NULL;
	}

#ifdef CONFIG_VHUGE_DAMON
	mode = READ_ONCE(vhuge_damon_mode);
	policy_mode = READ_ONCE(vhuge_damon_policy_mode);
	if (policy_mode == VHUGE_DAMON_POLICY_FIXED_BUDGET) {
		if (!READ_ONCE(vhuge_damon_budget_fcfs) ||
		    !vhuge_current_is_membound() ||
		    !vhuge_damon_budget_try_reserve(true)) {
			vhuge_stats.damon_alloc_reserve_bypass++;
			return NULL;
		}
		budget_reserved = true;
		goto take_entry;
	}
	if (mode != VHUGE_DAMON_IDLE &&
	    (!READ_ONCE(vhuge_damon_enabled) ||
	     !READ_ONCE(vhuge_damon_running))) {
		vhuge_stats.damon_alloc_reserve_bypass++;
		return NULL;
	}
	if (mode != VHUGE_DAMON_IDLE) {
		hot = vhuge_monitor_addr_hot(mm, addr & HPAGE_PMD_MASK);
		if (mode == VHUGE_DAMON_PRESSURE && !hot) {
			vhuge_stats.damon_alloc_cold_bypass++;
			return NULL;
		}
		if (mode == VHUGE_DAMON_SCOUT && !hot &&
		    READ_ONCE(vhuge_pool_count) <=
			    READ_ONCE(vhuge_damon_reserve_floor)) {
			vhuge_stats.damon_alloc_reserve_bypass++;
			return NULL;
		}
		if (hot)
			vhuge_stats.damon_alloc_hot++;
	}
take_entry:
#endif

	entry = vhuge_take_pool_entry(nid, true);
	if (!entry) {
#ifdef CONFIG_VHUGE_DAMON
		if (budget_reserved)
			vhuge_damon_budget_put();
#endif
		return NULL;
	}
	hp_index = pfn_to_hp_index(entry->pfn);
	if (!READ_ONCE(vhuge_enabled)) {
		ret = -EAGAIN;
		goto out_return_entry;
	}

#ifdef CONFIG_VHUGE_DAMON
	if (!budget_reserved &&
	    READ_ONCE(vhuge_damon_policy_mode) ==
			VHUGE_DAMON_POLICY_FIXED_BUDGET) {
		ret = -EAGAIN;
		goto out_return_entry;
	}
	if (budget_reserved) {
		ret = vhuge_damon_budget_tag(hp_index);
		if (ret)
			goto out_return_entry;
		budget_tagged = true;
	}
#endif

	ret = xa_insert(&vhuge_consumed_xa, hp_index, xa_mk_value(1),
			GFP_ATOMIC);
	if (ret)
		goto out_return_entry;
	vhuge_stats.consumed++;
	vhuge_prepare_allocated_page(entry->page, HPAGE_PMD_ORDER, gfp);
	folio = page_rmappable_folio(entry->page);
	kfree(entry);
	return folio;

out_return_entry:
#ifdef CONFIG_VHUGE_DAMON
	if (budget_tagged)
		vhuge_damon_budget_release_hp(hp_index);
	else if (budget_reserved)
		vhuge_damon_budget_put();
#endif
	if (ret != -EAGAIN)
		vhuge_stats.ownership_lost++;
	vhuge_return_entry_to_pool(entry, false);
	return NULL;
}

bool vhuge_capture_prepared_page(struct page *page, unsigned int order)
{
	struct vhuge_pool_entry *entry;
	unsigned long hp_index;
	unsigned long flags;
	bool marked;

	hp_index = pfn_to_hp_index(page_to_pfn(page));
#ifdef CONFIG_VHUGE_DAMON
	vhuge_damon_budget_release_hp(hp_index);
#endif
	vhuge_remove_owned_hp(hp_index, false);
	xa_lock_irqsave(&vhuge_consumed_xa, flags);
	__xa_erase(&vhuge_consumed_xa, hp_index);
	xa_unlock_irqrestore(&vhuge_consumed_xa, flags);

	xa_lock_irqsave(&vhuge_capture_xa, flags);
	marked = __xa_erase(&vhuge_capture_xa, hp_index) != NULL;
	xa_unlock_irqrestore(&vhuge_capture_xa, flags);
	if (!marked || !READ_ONCE(vhuge_enabled))
		return false;

	entry = kzalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry)
		return false;

	entry->page = page;
	entry->pfn = page_to_pfn(page);
	entry->nid = page_to_nid(page);

	spin_lock_irqsave(&vhuge_pool_lock, flags);
	if (!READ_ONCE(vhuge_enabled) ||
	    vhuge_pool_count >= READ_ONCE(vhuge_pool_max)) {
		spin_unlock_irqrestore(&vhuge_pool_lock, flags);
		kfree(entry);
		return false;
	}
	list_add_tail(&entry->list, &vhuge_pool);
	vhuge_pool_count++;
	vhuge_stats.captured++;
	spin_unlock_irqrestore(&vhuge_pool_lock, flags);

	return true;
}

void vhuge_note_allocated_folio(struct folio *folio, enum vhuge_hp_source source,
				struct mm_struct *mm)
{
	unsigned long hp_index;
	bool from_pool;

	if (folio_order(folio) != HPAGE_PMD_ORDER)
		return;

#ifdef CONFIG_VHUGE_DAMON
	if (READ_ONCE(vhuge_enabled) && !vhuge_is_daemon_mm(mm))
		vhuge_monitor_register_mm(mm);
#endif

	hp_index = pfn_to_hp_index(folio_pfn(folio));
	from_pool = xa_erase(&vhuge_consumed_xa, hp_index) != NULL;

	switch (source) {
	case VHUGE_HP_SRC_THP_FAULT:
		vhuge_stats.thp_fault_allocs++;
		break;
	case VHUGE_HP_SRC_KHUGEPAGED:
		vhuge_stats.khugepaged_allocs++;
		break;
	default:
		break;
	}

	if (from_pool && vhuge_add_owned_hp(folio, source, mm))
		vhuge_stats.ownership_lost++;
}

void vhuge_forget_folio(struct folio *folio)
{
	unsigned long hp_index = pfn_to_hp_index(folio_pfn(folio));

#ifdef CONFIG_VHUGE_DAMON
	vhuge_damon_budget_release_hp(hp_index);
#endif
	vhuge_remove_owned_hp(hp_index, true);
	xa_erase(&vhuge_consumed_xa, hp_index);
}

static int vhuge_mark_addr(struct mm_struct *mm, unsigned long addr)
{
	struct vm_area_struct *vma;
	spinlock_t *ptl;
	struct folio *folio;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmdp, pmd;
	unsigned long hp_index;
	int ret = -EINVAL;

	if (addr & ~HPAGE_PMD_MASK)
		return -EINVAL;

	mmap_read_lock(mm);
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start)
		goto out_unlock_mmap;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		goto out_unlock_mmap;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		goto out_unlock_mmap;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		goto out_unlock_mmap;

	pmdp = pmd_offset(pud, addr);
	ptl = pmd_lock(mm, pmdp);
	pmd = *pmdp;
	if (!pmd_present(pmd) || !pmd_trans_huge(pmd) || is_huge_zero_pmd(pmd)) {
		spin_unlock(ptl);
		goto out_unlock_mmap;
	}
	folio = pmd_folio(pmd);
	hp_index = pfn_to_hp_index(folio_pfn(folio));
	spin_unlock(ptl);

	ret = xa_err(xa_store(&vhuge_capture_xa, hp_index, xa_mk_value(1),
			      GFP_KERNEL));

out_unlock_mmap:
	mmap_read_unlock(mm);
	return ret;
}

static long vhuge_get_validate_batch(struct file *file, unsigned long arg)
{
	struct vhuge_file_ctx *ctx = file->private_data;
	struct vhuge_validate_batch req;
	struct vhuge_pool_entry *entry, *tmp;
	LIST_HEAD(local);
	unsigned long flags;
	u32 pool_after;
	u32 wanted, slot = 0;
	long ret;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.flags)
		return -EINVAL;

	wanted = min(req.max_entries, VHUGE_VALIDATE_MAX);
	req.nr_entries = 0;
	if (!wanted)
		goto out_copy;

	mutex_lock(&ctx->validating_lock);
	if (ctx->validating_count) {
		mutex_unlock(&ctx->validating_lock);
		return -EBUSY;
	}

	spin_lock_irqsave(&vhuge_pool_lock, flags);
	list_for_each_entry_safe(entry, tmp, &vhuge_pool, list) {
		if (req.nr_entries >= wanted)
			break;
		list_del(&entry->list);
		vhuge_pool_count--;
		list_add_tail(&entry->list, &local);
		req.nr_entries++;
	}
	pool_after = vhuge_pool_count;
	spin_unlock_irqrestore(&vhuge_pool_lock, flags);

	if (copy_to_user((void __user *)arg, &req, sizeof(req))) {
		spin_lock_irqsave(&vhuge_pool_lock, flags);
		list_splice_tail(&local, &vhuge_pool);
		vhuge_pool_count += req.nr_entries;
		spin_unlock_irqrestore(&vhuge_pool_lock, flags);
		mutex_unlock(&ctx->validating_lock);
		return -EFAULT;
	}

	list_for_each_entry(entry, &local, list) {
		vhuge_prepare_allocated_page(entry->page, HPAGE_PMD_ORDER,
					     GFP_TRANSHUGE);
		entry->allocated = true;
		entry->validation_slot = slot++;
	}

	list_splice_tail(&local, &ctx->validating);
	ctx->validating_count = req.nr_entries;
	if (req.nr_entries) {
		atomic_add(req.nr_entries, &vhuge_validation_in_progress);
		pr_debug("vhuge: validation batch start pid=%d comm=%s requested=%u granted=%u pool_after=%u in_progress=%u\n",
			task_pid_nr(current), current->comm, wanted,
			req.nr_entries, pool_after,
			atomic_read(&vhuge_validation_in_progress));
	}
	mutex_unlock(&ctx->validating_lock);
	return 0;

out_copy:
	ret = copy_to_user((void __user *)arg, &req, sizeof(req));
	return ret ? -EFAULT : 0;
}

static long vhuge_report_validate(struct file *file, unsigned long arg)
{
	struct vhuge_file_ctx *ctx = file->private_data;
	struct vhuge_validate_report req;
	struct vhuge_validate_result *results = NULL;
	struct vhuge_pool_entry *entry, *tmp;
	struct vhuge_pool_entry **by_slot = NULL;
	struct vhuge_validate_result __user *uresults;
	LIST_HEAD(return_list);
	LIST_HEAD(free_list);
	u32 real = 0, not_real = 0;
	u32 i;
	long ret = 0;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (req.flags || req.nr_results > VHUGE_VALIDATE_MAX)
		return -EINVAL;

	mutex_lock(&ctx->validating_lock);
	if (ctx->mapping_count) {
		mutex_unlock(&ctx->validating_lock);
		return -EBUSY;
	}
	if (req.nr_results != ctx->validating_count) {
		mutex_unlock(&ctx->validating_lock);
		return -EINVAL;
	}
	mutex_unlock(&ctx->validating_lock);

	uresults = (struct vhuge_validate_result __user *)(uintptr_t)req.results;
	if (!req.nr_results)
		return 0;

	results = memdup_user(uresults, req.nr_results * sizeof(*results));
	if (IS_ERR(results))
		return PTR_ERR(results);
	by_slot = kcalloc(req.nr_results, sizeof(*by_slot), GFP_KERNEL);
	if (!by_slot) {
		ret = -ENOMEM;
		goto out_free_arrays;
	}

	mutex_lock(&ctx->validating_lock);
	if (ctx->mapping_count) {
		ret = -EBUSY;
		goto out_unlock;
	}
	/* Check every slot before taking any entry off the list. */
	for (i = 0; i < req.nr_results; i++) {
		if (results[i].slot >= req.nr_results ||
		    by_slot[results[i].slot]) {
			ret = -EINVAL;
			goto out_unlock;
		}
		list_for_each_entry(entry, &ctx->validating, list) {
			if (entry->validation_slot == results[i].slot) {
				by_slot[results[i].slot] = entry;
				break;
			}
		}
		if (!by_slot[results[i].slot]) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}

	for (i = 0; i < req.nr_results; i++) {
		entry = by_slot[results[i].slot];
		list_del(&entry->list);
		ctx->validating_count--;
		atomic_dec(&vhuge_validation_in_progress);
		vhuge_stats.validation_checked++;
		if (results[i].is_real) {
			real++;
			list_add_tail(&entry->list, &return_list);
		} else {
			not_real++;
			vhuge_stats.validation_failed++;
			list_add_tail(&entry->list, &free_list);
		}
	}
	mutex_unlock(&ctx->validating_lock);

	list_for_each_entry_safe(entry, tmp, &return_list, list) {
		list_del(&entry->list);
		vhuge_return_entry_to_pool(entry, true);
	}
	list_for_each_entry_safe(entry, tmp, &free_list, list) {
		list_del(&entry->list);
		vhuge_stats.validation_released++;
		vhuge_free_entry(entry);
	}
	pr_debug("vhuge: validation report pid=%d comm=%s checked=%u real=%u not_real=%u in_progress=%u\n",
		task_pid_nr(current), current->comm, req.nr_results, real,
		not_real, atomic_read(&vhuge_validation_in_progress));
	goto out_free_arrays;

out_unlock:
	mutex_unlock(&ctx->validating_lock);
out_free_arrays:
	kfree(by_slot);
	kfree(results);
	return ret;
}

static long vhuge_set_daemon_status(unsigned long arg)
{
	struct vhuge_daemon_status status;

	if (copy_from_user(&status, (void __user *)arg, sizeof(status)))
		return -EFAULT;
	if (status.flags & ~VHUGE_DAEMON_STATUS_KNOWN_FLAGS)
		return -EINVAL;
	WRITE_ONCE(vhuge_daemon_status.next_refill_check_sec,
		   status.next_refill_check_sec);
	WRITE_ONCE(vhuge_daemon_status.next_validation_sec,
		   status.next_validation_sec);
	WRITE_ONCE(vhuge_daemon_status.last_refill_wanted,
		   status.last_refill_wanted);
	WRITE_ONCE(vhuge_daemon_status.last_refill_captured,
		   status.last_refill_captured);
	WRITE_ONCE(vhuge_daemon_status.last_refill_success_pct,
		   status.last_refill_success_pct);
	WRITE_ONCE(vhuge_daemon_status.last_validation_batch,
		   status.last_validation_batch);
	WRITE_ONCE(vhuge_daemon_status.flags, status.flags);
	WRITE_ONCE(vhuge_daemon_status.last_refill_seq,
		   status.last_refill_seq);
#ifdef CONFIG_VHUGE_DAMON
	vhuge_damon_policy_update(&status);
#endif
	return 0;
}

static long vhuge_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct vhuge_mark_addrs req;
	u64 __user *uaddrs;
	u64 addr;
	u32 i;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!READ_ONCE(vhuge_enabled))
		return -EAGAIN;
	if (cmd == VHUGE_IOC_GET_VALIDATE_BATCH)
		return vhuge_get_validate_batch(file, arg);
	if (cmd == VHUGE_IOC_REPORT_VALIDATE)
		return vhuge_report_validate(file, arg);
	if (cmd == VHUGE_IOC_SET_DAEMON_STATUS)
		return vhuge_set_daemon_status(arg);
	if (cmd != VHUGE_IOC_MARK_REAL_ADDRS)
		return -EINVAL;
	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;
	if (!req.nr_addrs || req.nr_addrs > VHUGE_MARK_ADDRS_MAX || req.flags)
		return -EINVAL;

	uaddrs = (u64 __user *)(uintptr_t)req.addrs;
	mutex_lock(&vhuge_enable_lock);
	if (!READ_ONCE(vhuge_enabled)) {
		mutex_unlock(&vhuge_enable_lock);
		return -EAGAIN;
	}
	for (i = 0; i < req.nr_addrs; i++) {
		if (copy_from_user(&addr, &uaddrs[i], sizeof(addr))) {
			ret = -EFAULT;
			break;
		}
		ret = vhuge_mark_addr(current->mm, (unsigned long)addr);
		if (ret) {
			vhuge_stats.mark_fail++;
			break;
		}
		vhuge_stats.mark_success++;
	}
	mutex_unlock(&vhuge_enable_lock);

	return ret;
}

static vm_fault_t vhuge_validate_huge_fault(struct vm_fault *vmf,
					    unsigned int order)
{
	struct vhuge_file_ctx *ctx = vmf->vma->vm_private_data;
	struct vhuge_pool_entry *entry;
	unsigned long offset;
	u32 slot;
	vm_fault_t ret = VM_FAULT_SIGBUS;

	if (order != PMD_ORDER)
		return VM_FAULT_FALLBACK;

	offset = (vmf->address & PMD_MASK) - vmf->vma->vm_start;
	slot = offset >> HPAGE_PMD_SHIFT;

	mutex_lock(&ctx->validating_lock);
	list_for_each_entry(entry, &ctx->validating, list) {
		if (entry->validation_slot != slot)
			continue;
		ret = vmf_insert_pfn_pmd(vmf, pfn_to_pfn_t(entry->pfn),
					 false);
		if (ret & VM_FAULT_ERROR)
			pr_warn_ratelimited("vhuge: validation PMD insert failed ret=0x%x slot=%u pfn=%lu pid=%d comm=%s\n",
					    ret, slot, entry->pfn,
					    task_pid_nr(current), current->comm);
		break;
	}
	mutex_unlock(&ctx->validating_lock);
	if (ret == VM_FAULT_SIGBUS)
		pr_warn_ratelimited("vhuge: validation fault missing slot=%u addr=%lx pid=%d comm=%s\n",
				    slot, vmf->address, task_pid_nr(current),
				    current->comm);
	return ret;
}

static vm_fault_t vhuge_validate_fault(struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static void vhuge_validate_vma_open(struct vm_area_struct *vma)
{
	struct vhuge_file_ctx *ctx = vma->vm_private_data;

	refcount_inc(&ctx->refs);
	mutex_lock(&ctx->validating_lock);
	ctx->mapping_count++;
	mutex_unlock(&ctx->validating_lock);
}

static void vhuge_validate_vma_close(struct vm_area_struct *vma)
{
	struct vhuge_file_ctx *ctx = vma->vm_private_data;
	bool return_entries = false;

	mutex_lock(&ctx->validating_lock);
	if (ctx->mapping_count)
		ctx->mapping_count--;
	return_entries = ctx->fd_closed && !ctx->mapping_count;
	mutex_unlock(&ctx->validating_lock);
	if (return_entries) {
		vhuge_return_validation_entries(ctx);
		vhuge_unregister_daemon_ctx(ctx);
	}
	vhuge_ctx_put(ctx);
}

static const struct vm_operations_struct vhuge_validate_vm_ops = {
	.open = vhuge_validate_vma_open,
	.close = vhuge_validate_vma_close,
	.fault = vhuge_validate_fault,
	.huge_fault = vhuge_validate_huge_fault,
};

static int vhuge_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct vhuge_file_ctx *ctx = file->private_data;
	unsigned long len = vma->vm_end - vma->vm_start;
	int ret = 0;

	if (len > VHUGE_VALIDATE_MAX * HPAGE_PMD_SIZE)
		return -EINVAL;
	if ((vma->vm_start | vma->vm_end) & (HPAGE_PMD_SIZE - 1))
		return -EINVAL;
	if (!(vma->vm_flags & VM_SHARED))
		return -EINVAL;

	mutex_lock(&ctx->validating_lock);
	if (!ctx->validating_count || ctx->mapping_count ||
	    len != (unsigned long)ctx->validating_count * HPAGE_PMD_SIZE) {
		ret = -EINVAL;
		goto out_unlock;
	}

	vma->vm_private_data = ctx;
	refcount_inc(&ctx->refs);
	ctx->mapping_count++;
	vma->vm_ops = &vhuge_validate_vm_ops;
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP |
		     VM_HUGEPAGE);
	pr_debug("vhuge: validation mmap pid=%d comm=%s start=%lx len=%lu flags=%lx\n",
		task_pid_nr(current), current->comm, vma->vm_start,
		vma->vm_end - vma->vm_start, vma->vm_flags);
out_unlock:
	mutex_unlock(&ctx->validating_lock);
	return ret;
}

static int vhuge_open(struct inode *inode, struct file *file)
{
	struct vhuge_file_ctx *ctx;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (atomic_cmpxchg(&vhuge_daemon_open, 0, 1))
		return -EBUSY;
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		atomic_set(&vhuge_daemon_open, 0);
		return -ENOMEM;
	}
	INIT_LIST_HEAD(&ctx->validating);
	mutex_init(&ctx->validating_lock);
	refcount_set(&ctx->refs, 1);
	ctx->mm = current->mm;
	ctx->daemon_registered = true;
	file->private_data = ctx;

	mmgrab(ctx->mm);
	WRITE_ONCE(vhuge_daemon_mm, ctx->mm);
	pr_info("vhuge: daemon open pid=%d comm=%s mm=%p\n",
		task_pid_nr(current), current->comm, ctx->mm);
	return nonseekable_open(inode, file);
}

static int vhuge_release(struct inode *inode, struct file *file)
{
	struct vhuge_file_ctx *ctx = file->private_data;
	bool return_entries;

	mutex_lock(&ctx->validating_lock);
	ctx->fd_closed = true;
	pr_info("vhuge: daemon release pid=%d comm=%s mm=%p validating=%u mappings=%u\n",
		task_pid_nr(current), current->comm, ctx->mm,
		ctx->validating_count, ctx->mapping_count);
	return_entries = !ctx->mapping_count;
	mutex_unlock(&ctx->validating_lock);
	if (return_entries) {
		vhuge_return_validation_entries(ctx);
		vhuge_unregister_daemon_ctx(ctx);
	}
	vhuge_ctx_put(ctx);
	file->private_data = NULL;
	return 0;
}

static const struct file_operations vhuge_fops = {
	.owner = THIS_MODULE,
	.open = vhuge_open,
	.release = vhuge_release,
	.unlocked_ioctl = vhuge_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.mmap = vhuge_mmap,
	.get_unmapped_area = thp_get_unmapped_area,
	.llseek = noop_llseek,
};

static struct miscdevice vhuge_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = VHUGE_DEV_NAME,
	.fops = &vhuge_fops,
	.mode = 0600,
};

#ifdef CONFIG_VHUGE_DAMON
static bool vhuge_damon_vma_allowed(struct vm_area_struct *vma)
{
	vm_flags_t skip = VM_IO | VM_PFNMAP | VM_MIXEDMAP | VM_HUGETLB;

	return (vma->vm_flags & VM_READ) && vma_is_anonymous(vma) &&
	       !(vma->vm_flags & skip);
}

static void vhuge_damon_classify_pmd(struct mm_struct *mm,
				     struct vm_area_struct *vma,
				     unsigned long addr,
				     struct vhuge_damon_scan_result *result)
{
	spinlock_t *ptl;
	struct folio *folio;
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmdp, pmd;
	unsigned long hp_index;
	bool real;
	bool queued_ref = false;

	pgd = pgd_offset(mm, addr);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return;
	p4d = p4d_offset(pgd, addr);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return;
	pud = pud_offset(p4d, addr);
	if (pud_none(*pud) || unlikely(pud_bad(*pud)))
		return;

	pmdp = pmd_offset(pud, addr);
	ptl = pmd_trans_huge_lock(pmdp, vma);
	if (!ptl)
		return;
	pmd = *pmdp;
	if (!pmd_present(pmd) || !pmd_trans_huge(pmd) ||
	    is_huge_zero_pmd(pmd)) {
		spin_unlock(ptl);
		return;
	}
	folio = pmd_folio(pmd);
	hp_index = pfn_to_hp_index(folio_pfn(folio));
	real = vhuge_owned_hp_lookup(hp_index);
	if (!real && folio_entire_mapcount(folio) == 1 &&
	    !vhuge_is_daemon_mm(mm)) {
		folio_get(folio);
		mmget(mm);
		queued_ref = true;
	}
	spin_unlock(ptl);

	result->transhuge++;
	result->hot_mapped++;
	if (real) {
		result->hot_real++;
	} else if (vhuge_probe_fake_recent(hp_index)) {
		result->hot_fake++;
		vhuge_stats.damon_probe_cooldown_skip++;
		if (queued_ref) {
			vhuge_migrate_queue(mm, folio, addr, hp_index, result);
			queued_ref = false;
		}
	} else {
		result->hot_unknown++;
		if (queued_ref) {
			vhuge_probe_queue(mm, folio, addr, hp_index, result);
			queued_ref = false;
		}
	}
	if (queued_ref) {
		mmput(mm);
		folio_put(folio);
	}
}

static void vhuge_damon_classify_addr(struct pid *pid, unsigned long addr,
				      struct vhuge_damon_scan_result *result)
{
	struct task_struct *task;
	struct mm_struct *mm;
	struct vm_area_struct *vma;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		return;
	if (!mmap_read_trylock(mm))
		goto out_mm;
	vma = find_vma(mm, addr);
	if (!vma || addr < vma->vm_start ||
	    addr + HPAGE_PMD_SIZE > vma->vm_end ||
	    !vhuge_damon_vma_allowed(vma))
		goto out_unlock;
	result->slots_checked++;
	vhuge_damon_classify_pmd(mm, vma, addr, result);
out_unlock:
	mmap_read_unlock(mm);
out_mm:
	mmput(mm);
}

static int vhuge_hot_candidate_cmp(const void *a, const void *b)
{
	const struct vhuge_hot_candidate *ca = a;
	const struct vhuge_hot_candidate *cb = b;

	if (ca->score < cb->score)
		return 1;
	if (ca->score > cb->score)
		return -1;
	if (ca->addr < cb->addr)
		return -1;
	if (ca->addr > cb->addr)
		return 1;
	return 0;
}

static struct vhuge_hot_candidate *vhuge_hot_candidates_snapshot(u32 *nr)
{
	struct vhuge_hot_candidate *candidates;
	struct vhuge_monitor_target *target;
	unsigned long index;
	void *entry;
	u32 count = 0, copied = 0;

	*nr = 0;
	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry(target, &vhuge_monitor_targets, list) {
		xa_for_each(&target->hot_pmds, index, entry) {
			if (++count >= READ_ONCE(vhuge_damon_hot_pmd_max))
				goto counted;
		}
	}
counted:
	mutex_unlock(&vhuge_monitor_lock);
	if (!count)
		return NULL;

	candidates = kcalloc(count, sizeof(*candidates), GFP_KERNEL);
	if (!candidates)
		return NULL;
	mutex_lock(&vhuge_monitor_lock);
	list_for_each_entry(target, &vhuge_monitor_targets, list) {
		xa_for_each(&target->hot_pmds, index, entry) {
			if (copied >= count)
				goto copied;
			candidates[copied].pid = get_pid(target->pid);
			candidates[copied].addr = index << HPAGE_PMD_SHIFT;
			candidates[copied].score = xa_to_value(entry) - 1;
			copied++;
		}
	}
copied:
	mutex_unlock(&vhuge_monitor_lock);
	*nr = copied;
	sort(candidates, copied, sizeof(*candidates),
	     vhuge_hot_candidate_cmp, NULL);
	return candidates;
}

static void vhuge_damon_record_last_scan(struct vhuge_damon_scan_result *result)
{
	WRITE_ONCE(vhuge_stats.damon_scan_last_checked, result->slots_checked);
	WRITE_ONCE(vhuge_stats.damon_scan_last_transhuge, result->transhuge);
	WRITE_ONCE(vhuge_stats.damon_scan_last_hot_mapped, result->hot_mapped);
	WRITE_ONCE(vhuge_stats.damon_scan_last_hot_real, result->hot_real);
	WRITE_ONCE(vhuge_stats.damon_scan_last_hot_unknown,
		   result->hot_unknown);
	WRITE_ONCE(vhuge_stats.damon_scan_last_hot_fake, result->hot_fake);
}

static void vhuge_damon_classify_workfn(struct work_struct *work)
{
	struct vhuge_damon_scan_result result = {};
	struct vhuge_hot_candidate *candidates;
	u32 nr_candidates = 0, i;
	ktime_t started = ktime_get();
	u64 elapsed_us;

	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_enabled) ||
	    !READ_ONCE(vhuge_damon_running))
		return;
	if (!vhuge_monitor_has_live_target()) {
		mod_delayed_work(system_wq, &vhuge_damon_rebuild_work, 0);
		return;
	}

	if (READ_ONCE(vhuge_damon_mode) != VHUGE_DAMON_PRESSURE)
		goto update_mode;
	if (READ_ONCE(vhuge_damon_policy_mode) ==
			VHUGE_DAMON_POLICY_FIXED_BUDGET &&
	    !vhuge_damon_budget_has_room())
		goto update_mode;
	candidates = vhuge_hot_candidates_snapshot(&nr_candidates);
	if (!candidates) {
		vhuge_stats.damon_scan_skipped_no_hot++;
		goto update_mode;
	}
	for (i = 0; i < nr_candidates; i++) {
		vhuge_damon_classify_addr(candidates[i].pid,
					   candidates[i].addr, &result);
		put_pid(candidates[i].pid);
		candidates[i].pid = NULL;
		if (result.probes_queued >=
				READ_ONCE(vhuge_damon_probe_max_per_scan) &&
		    (!READ_ONCE(vhuge_pool_count) ||
		     result.migrations_queued >=
				READ_ONCE(vhuge_damon_migrate_max_per_scan))) {
			i++;
			break;
		}
	}
	for (; i < nr_candidates; i++)
		put_pid(candidates[i].pid);
	kfree(candidates);
	vhuge_stats.damon_scan_passes++;

	vhuge_stats.damon_scan_slots_checked += result.slots_checked;
	vhuge_stats.damon_scan_transhuge += result.transhuge;
	vhuge_stats.damon_scan_hot_mapped += result.hot_mapped;
	vhuge_stats.damon_scan_hot_real += result.hot_real;
	vhuge_stats.damon_scan_hot_unknown += result.hot_unknown;
	vhuge_stats.damon_scan_hot_fake += result.hot_fake;
	if (result.probe_budget_hit)
		vhuge_stats.damon_probe_budget_hit++;
	if (result.migrate_limit_hit)
		vhuge_stats.damon_migrate_limit_hit++;

update_mode:
	vhuge_damon_record_last_scan(&result);
	elapsed_us = ktime_us_delta(ktime_get(), started);
	if (nr_candidates)
		update_timing(elapsed_us, &vhuge_stats.damon_last_scan_us,
			      &vhuge_stats.damon_max_scan_us);

	if (READ_ONCE(vhuge_damon_running) &&
	    READ_ONCE(vhuge_damon_enabled) && READ_ONCE(vhuge_enabled))
		queue_delayed_work(vhuge_damon_wq, &vhuge_damon_classify_work,
				   msecs_to_jiffies(VHUGE_DAMON_CLASSIFY_INTERVAL_SEC *
						    1000));
}

static int vhuge_damon_start_locked(void)
{
	struct damon_attrs attrs = {
		.sample_interval = READ_ONCE(vhuge_damon_sample_us),
		.aggr_interval = READ_ONCE(vhuge_damon_aggr_us),
		.ops_update_interval = READ_ONCE(vhuge_damon_update_us),
		.min_nr_regions = READ_ONCE(vhuge_damon_min_regions),
		.max_nr_regions = READ_ONCE(vhuge_damon_max_regions),
	};
	struct damon_ctx *ctx;
	struct vhuge_monitor_target *monitor;
	struct damon_target *target;
	u32 nr_targets = 0;
	int ret;

	if (vhuge_damon_running)
		return 0;
	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_enabled))
		return 0;

	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;
	ret = damon_select_ops(ctx, DAMON_OPS_VADDR);
	if (ret)
		goto err_ctx;
	ret = damon_set_attrs(ctx, &attrs);
	if (ret)
		goto err_ctx;

	mutex_lock(&vhuge_monitor_lock);
	vhuge_monitor_reap_dead_locked();
	list_for_each_entry(monitor, &vhuge_monitor_targets, list) {
		target = damon_new_target();
		if (!target) {
			ret = -ENOMEM;
			goto err_targets_locked;
		}
		target->pid = get_pid(monitor->pid);
		damon_add_target(ctx, target);
		nr_targets++;
	}
	if (!nr_targets) {
		ret = -ESRCH;
		goto err_targets_locked;
	}
	vhuge_damon_targets_dirty = false;
	mutex_unlock(&vhuge_monitor_lock);
	ctx->callback.after_aggregation = vhuge_damon_after_aggregation;

	vhuge_damon_ctx = ctx;
	ret = damon_start(&vhuge_damon_ctx, 1, false);
	if (ret)
		goto err_running;

	vhuge_damon_running = true;
	queue_delayed_work(vhuge_damon_wq, &vhuge_damon_classify_work,
			   msecs_to_jiffies(VHUGE_DAMON_CLASSIFY_INTERVAL_SEC *
					    1000));
	mutex_lock(&vhuge_monitor_lock);
	if (vhuge_damon_targets_dirty)
		mod_delayed_work(system_wq, &vhuge_damon_rebuild_work, 0);
	mutex_unlock(&vhuge_monitor_lock);
	pr_info("vhuge: DAMON VADDR started targets=%u sample_us=%lu aggr_us=%lu regions=%lu-%lu hot_threshold_bp=%u\n",
		nr_targets,
		attrs.sample_interval, attrs.aggr_interval,
		attrs.min_nr_regions, attrs.max_nr_regions,
		READ_ONCE(vhuge_damon_hot_threshold_bp));
	return 0;

err_targets_locked:
	mutex_unlock(&vhuge_monitor_lock);
err_running:
	vhuge_damon_ctx = NULL;
err_ctx:
	damon_for_each_target(target, ctx)
		put_pid(target->pid);
	damon_destroy_ctx(ctx);
	if (ret != -ESRCH)
		pr_warn("vhuge: DAMON VADDR start failed ret=%d\n", ret);
	return ret;
}

static void vhuge_damon_stop_locked(void)
{
	struct vhuge_damon_scan_result empty = {};
	struct damon_ctx *ctx = vhuge_damon_ctx;
	struct damon_target *target;
	bool was_running = vhuge_damon_running;

	vhuge_damon_running = false;
	cancel_delayed_work_sync(&vhuge_damon_classify_work);
	kthread_flush_worker(vhuge_probe_worker);
	kthread_flush_worker(vhuge_migrate_worker);
	if (ctx) {
		if (was_running)
			damon_stop(&ctx, 1);
		vhuge_damon_ctx = NULL;
		damon_for_each_target(target, ctx)
			put_pid(target->pid);
		damon_destroy_ctx(ctx);
	}
	vhuge_monitor_clear_hot();
	vhuge_damon_record_last_scan(&empty);
	vhuge_probe_clear();
	vhuge_migrate_clear();
	if (was_running)
		pr_info("vhuge: DAMON VADDR stopped\n");
}

static int vhuge_damon_restart_if_running(void)
{
	int ret = 0;

	mutex_lock(&vhuge_damon_lock);
	if (vhuge_damon_running) {
		vhuge_damon_stop_locked();
		ret = vhuge_damon_start_locked();
	}
	mutex_unlock(&vhuge_damon_lock);
	return ret;
}

static void vhuge_damon_rebuild_workfn(struct work_struct *work)
{
	enum vhuge_damon_mode mode;
	unsigned long started;
	bool dirty;
	int ret;

	mutex_lock(&vhuge_damon_lock);
	mutex_lock(&vhuge_monitor_lock);
	dirty = vhuge_damon_targets_dirty;
	mutex_unlock(&vhuge_monitor_lock);
	if (!dirty)
		goto out_unlock;
	if (!vhuge_damon_running) {
		if (READ_ONCE(vhuge_damon_policy_mode) ==
				VHUGE_DAMON_POLICY_FIXED_BUDGET &&
		    READ_ONCE(vhuge_damon_mode) != VHUGE_DAMON_IDLE)
			vhuge_damon_start_locked();
		goto out_unlock;
	}

	mode = READ_ONCE(vhuge_damon_mode);
	started = READ_ONCE(vhuge_damon_started_jiffies);
	vhuge_damon_stop_locked();
	ret = vhuge_damon_start_locked();
	if (!ret && vhuge_damon_running) {
		WRITE_ONCE(vhuge_damon_mode, mode);
		WRITE_ONCE(vhuge_damon_started_jiffies, started);
		vhuge_stats.damon_target_rebuilds++;
	}
out_unlock:
	mutex_unlock(&vhuge_damon_lock);
}

static u32 vhuge_damon_stop_watermark(void)
{
	u32 low = READ_ONCE(vhuge_pool_low);
	u32 target = READ_ONCE(vhuge_pool_target);
	u32 pct = READ_ONCE(vhuge_damon_stop_watermark_pct);
	u64 range;

	if (target <= low)
		return low;

	range = (u64)(target - low) * min(pct, 100U);
	return low + DIV_ROUND_UP_ULL(range, 100);
}

static bool vhuge_damon_min_runtime_elapsed_locked(void)
{
	u32 min_runtime = READ_ONCE(vhuge_damon_min_runtime_sec);

	return !min_runtime ||
		       time_after_eq(jiffies, READ_ONCE(vhuge_damon_started_jiffies) +
			     min_runtime * HZ);
}

static bool vhuge_has_owned_real_pages(void)
{
	unsigned long flags;
	bool owned;

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	owned = !list_empty(&vhuge_owner_list);
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);
	return owned;
}

static int vhuge_damon_fixed_arm_locked(void)
{
	unsigned long flags;

	if (vhuge_damon_budget_in_use() || vhuge_has_owned_real_pages())
		return -EBUSY;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	vhuge_damon_policy_mode = VHUGE_DAMON_POLICY_FIXED_BUDGET;
	vhuge_damon_mode = VHUGE_DAMON_SCOUT;
	vhuge_damon_fixed_aggr_count = 0;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);

	vhuge_damon_stop_locked();
	if (vhuge_has_owned_real_pages()) {
		spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
		vhuge_damon_policy_mode = VHUGE_DAMON_POLICY_AUTO;
		vhuge_damon_mode = VHUGE_DAMON_IDLE;
		spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
		vhuge_damon_policy_reset_locked(false);
		return -EBUSY;
	}
	vhuge_monitor_clear();
	vhuge_damon_budget_clear();
	WRITE_ONCE(vhuge_damon_fixed_aggr_count, 0);
	WRITE_ONCE(vhuge_damon_refill_failures, 0);
	WRITE_ONCE(vhuge_damon_last_refill_seq, 0);
	WRITE_ONCE(vhuge_damon_started_jiffies, 0);
	WRITE_ONCE(vhuge_damon_reserve_floor,
		   READ_ONCE(vhuge_pool_count));
	WRITE_ONCE(vhuge_damon_mode, VHUGE_DAMON_SCOUT);
	vhuge_stats.damon_fixed_armed++;
	pr_info("vhuge: fixed-budget policy armed budget=%u scout_aggregations=%u pool=%u\n",
		READ_ONCE(vhuge_damon_hot_budget_pages),
		READ_ONCE(vhuge_damon_fixed_scout_aggrs),
		READ_ONCE(vhuge_pool_count));
	return 0;
}

static int vhuge_damon_auto_mode_locked(void)
{
	unsigned long flags;

	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	if (vhuge_damon_hot_budget_in_use) {
		spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
		return -EBUSY;
	}
	vhuge_damon_mode = VHUGE_DAMON_SCOUT;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	vhuge_damon_stop_locked();
	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	if (vhuge_damon_hot_budget_in_use) {
		spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
		return -EBUSY;
	}
	vhuge_damon_policy_mode = VHUGE_DAMON_POLICY_AUTO;
	vhuge_damon_mode = VHUGE_DAMON_IDLE;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	vhuge_damon_budget_clear();
	vhuge_damon_policy_reset_locked(false);
	WRITE_ONCE(vhuge_damon_last_refill_seq,
		   READ_ONCE(vhuge_daemon_status.last_refill_seq));
	return 0;
}

static void vhuge_damon_policy_reset_locked(bool stop_running)
{
	enum vhuge_damon_mode mode = READ_ONCE(vhuge_damon_mode);

	if (stop_running && vhuge_damon_running)
		vhuge_damon_stop_locked();
	WRITE_ONCE(vhuge_damon_refill_failures, 0);
	WRITE_ONCE(vhuge_damon_last_refill_seq, 0);
	WRITE_ONCE(vhuge_damon_started_jiffies, 0);
	WRITE_ONCE(vhuge_damon_reserve_floor, 0);
	WRITE_ONCE(vhuge_damon_mode, VHUGE_DAMON_IDLE);
	if (stop_running) {
		if (mode == VHUGE_DAMON_PRESSURE)
			vhuge_stats.damon_policy_stopped++;
		else if (mode == VHUGE_DAMON_SCOUT)
			vhuge_stats.damon_scout_recovered++;
	}
}

static void vhuge_damon_policy_update_locked(
		const struct vhuge_daemon_status *status)
{
	bool attempted = status->flags & VHUGE_DAEMON_STATUS_REFILL_ATTEMPTED;
	bool fill_to = status->flags & VHUGE_DAEMON_STATUS_REFILL_FILL_TO;
	u32 pool_current = READ_ONCE(vhuge_pool_count);
	u32 low = READ_ONCE(vhuge_pool_low);
	u32 stop = vhuge_damon_stop_watermark();
	u32 failures = READ_ONCE(vhuge_damon_refill_failures);
	enum vhuge_damon_mode mode = READ_ONCE(vhuge_damon_mode);
	int ret;

	if (!READ_ONCE(vhuge_enabled) || !READ_ONCE(vhuge_damon_enabled))
		return;
	if (READ_ONCE(vhuge_damon_policy_mode) ==
	    VHUGE_DAMON_POLICY_FIXED_BUDGET)
		return;

	if (vhuge_damon_running && mode == VHUGE_DAMON_PRESSURE &&
	    pool_current >= stop &&
	    vhuge_damon_min_runtime_elapsed_locked()) {
		vhuge_damon_stop_locked();
		vhuge_damon_policy_reset_locked(false);
		vhuge_stats.damon_policy_stopped++;
		pr_info("vhuge: DAMON pressure policy stopped pool=%u stop=%u\n",
			pool_current, stop);
		return;
	}

	if (!attempted || !status->last_refill_seq ||
	    status->last_refill_seq == vhuge_damon_last_refill_seq)
		return;
	WRITE_ONCE(vhuge_damon_last_refill_seq, status->last_refill_seq);

	if (fill_to) {
		vhuge_stats.damon_policy_fill_to_ignored++;
		return;
	}

	if (pool_current < low) {
		failures++;
		WRITE_ONCE(vhuge_damon_refill_failures, failures);
		vhuge_stats.damon_policy_refill_failures++;
	} else {
		if (failures)
			vhuge_stats.damon_policy_refill_recovered++;
		failures = 0;
		WRITE_ONCE(vhuge_damon_refill_failures, 0);
		if (mode == VHUGE_DAMON_SCOUT) {
			vhuge_damon_stop_locked();
			vhuge_damon_policy_reset_locked(false);
			vhuge_stats.damon_scout_recovered++;
			pr_info("vhuge: DAMON VADDR scout stopped after refill recovery pool=%u low=%u\n",
				pool_current, low);
		}
		return;
	}

	if (!vhuge_damon_running) {
		ret = vhuge_damon_start_locked();
		if (ret) {
			if (ret != -ESRCH)
				pr_warn("vhuge: DAMON VADDR scout start failed ret=%d\n",
					ret);
			return;
		}
		if (!vhuge_damon_running)
			return;
		WRITE_ONCE(vhuge_damon_reserve_floor,
			   max(pool_current,
			       READ_ONCE(vhuge_damon_migration_reserve)));
		WRITE_ONCE(vhuge_damon_mode, VHUGE_DAMON_SCOUT);
		vhuge_stats.damon_scout_started++;
		mode = VHUGE_DAMON_SCOUT;
		pr_info("vhuge: DAMON VADDR scout started after refill failure pool=%u low=%u\n",
			pool_current, low);
	}

	if (mode == VHUGE_DAMON_SCOUT &&
	    failures >= READ_ONCE(vhuge_damon_pressure_failures)) {
		WRITE_ONCE(vhuge_damon_mode, VHUGE_DAMON_PRESSURE);
		WRITE_ONCE(vhuge_damon_started_jiffies, jiffies);
		vhuge_stats.damon_policy_started++;
		pr_info("vhuge: DAMON VADDR pressure mode started after %u failed refills pool=%u low=%u stop=%u min_runtime_sec=%u\n",
			failures, pool_current, low, stop,
			READ_ONCE(vhuge_damon_min_runtime_sec));
	}
}

static void vhuge_damon_policy_update(
		const struct vhuge_daemon_status *status)
{
	mutex_lock(&vhuge_damon_lock);
	vhuge_damon_policy_update_locked(status);
	mutex_unlock(&vhuge_damon_lock);
}
#endif

static int owner_cmp(const void *a, const void *b)
{
	const struct vhuge_proc_owner *oa = a;
	const struct vhuge_proc_owner *ob = b;

	if (oa->real_hps != ob->real_hps)
		return oa->real_hps < ob->real_hps ? 1 : -1;
	if (oa->tgid != ob->tgid)
		return oa->tgid > ob->tgid ? 1 : -1;
	return strncmp(oa->comm, ob->comm, TASK_COMM_LEN);
}

static int vhuge_proc_show(struct seq_file *m, void *v)
{
	struct vhuge_proc_owner *rows = NULL;
	struct vhuge_mm_owner *owner;
	unsigned long flags;
	u32 owner_count = 0, row_count = 0, i, j;
	bool verbose = READ_ONCE(vhuge_proc_verbosity) > 0;
#ifdef CONFIG_VHUGE_DAMON
	u32 budget_in_use = vhuge_damon_budget_in_use();
	u32 budget_pages = READ_ONCE(vhuge_damon_hot_budget_pages);
#endif

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	list_for_each_entry(owner, &vhuge_owner_list, list)
		owner_count++;
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);

	if (owner_count) {
		rows = kcalloc(owner_count, sizeof(*rows), GFP_KERNEL);
		if (!rows)
			return -ENOMEM;
	}

	spin_lock_irqsave(&vhuge_owner_lock, flags);
	list_for_each_entry(owner, &vhuge_owner_list, list) {
		if (row_count >= owner_count)
			break;
		for (j = 0; j < row_count; j++) {
			if (rows[j].tgid == owner->tgid &&
			    !strncmp(rows[j].comm, owner->comm, TASK_COMM_LEN))
				break;
		}
		if (j == row_count) {
			rows[j].tgid = owner->tgid;
			strscpy(rows[j].comm, owner->comm,
				sizeof(rows[j].comm));
			row_count++;
		}
		rows[j].real_hps += owner->real_hps;
		rows[j].thp_fault_hps += owner->thp_fault_hps;
		rows[j].khugepaged_hps += owner->khugepaged_hps;
		rows[j].damon_probe_hps += owner->damon_probe_hps;
		rows[j].migrated_hps += owner->migrated_hps;
	}
	spin_unlock_irqrestore(&vhuge_owner_lock, flags);
	if (row_count > 1)
		sort(rows, row_count, sizeof(*rows), owner_cmp, NULL);

	seq_printf(m, "enabled: %u proc_verbosity=%u\n",
		   READ_ONCE(vhuge_enabled) ? 1 : 0,
		   READ_ONCE(vhuge_proc_verbosity));
	seq_printf(m, "pool: current=%u low=%u target=%u max=%u fill_to=%u\n",
		   READ_ONCE(vhuge_pool_count), READ_ONCE(vhuge_pool_low),
		   READ_ONCE(vhuge_pool_target), READ_ONCE(vhuge_pool_max),
		   READ_ONCE(vhuge_pool_fill_to));
	seq_printf(m, "refill: next_check_sec=%u last_wanted=%u last_captured=%u last_success_pct=%u\n",
		   READ_ONCE(vhuge_daemon_status.next_refill_check_sec),
		   READ_ONCE(vhuge_daemon_status.last_refill_wanted),
		   READ_ONCE(vhuge_daemon_status.last_refill_captured),
		   READ_ONCE(vhuge_daemon_status.last_refill_success_pct));
	seq_printf(m, "validation: next_check_sec=%u batch_limit=%u last_batch=%u in_progress=%u\n",
		   READ_ONCE(vhuge_daemon_status.next_validation_sec),
		   VHUGE_VALIDATE_MAX,
		   READ_ONCE(vhuge_daemon_status.last_validation_batch),
		   atomic_read(&vhuge_validation_in_progress));
#ifdef CONFIG_VHUGE_DAMON
	seq_printf(m, "damon: enabled=%u running=%u policy_mode=%u mode=%u targets=%u hot_virtual_pmds=%llu\n",
		   READ_ONCE(vhuge_damon_enabled) ? 1 : 0,
		   READ_ONCE(vhuge_damon_running) ? 1 : 0,
		   READ_ONCE(vhuge_damon_policy_mode),
		   READ_ONCE(vhuge_damon_mode),
		   READ_ONCE(vhuge_monitor_target_count),
		   vhuge_stats.damon_hot_pmd_ranges);
	seq_printf(m, "damon_policy: failures=%u threshold=%u stop_watermark=%u stop_pct=%u min_runtime_sec=%u migration_reserve=%u reserve_floor=%u last_refill_seq=%u\n",
		   READ_ONCE(vhuge_damon_refill_failures),
		   READ_ONCE(vhuge_damon_pressure_failures),
		   vhuge_damon_stop_watermark(),
		   READ_ONCE(vhuge_damon_stop_watermark_pct),
		   READ_ONCE(vhuge_damon_min_runtime_sec),
		   READ_ONCE(vhuge_damon_migration_reserve),
		   READ_ONCE(vhuge_damon_reserve_floor),
		   READ_ONCE(vhuge_damon_last_refill_seq));
	seq_printf(m, "fixed_budget: pages=%u in_use=%u remaining=%u scout_aggregations=%u/%u\n",
		   budget_pages, budget_in_use,
		   budget_pages > budget_in_use ?
			budget_pages - budget_in_use : 0,
		   READ_ONCE(vhuge_damon_fixed_aggr_count),
		   READ_ONCE(vhuge_damon_fixed_scout_aggrs));
	seq_printf(m, "classification: checked=%llu transhuge=%llu hot_mapped=%llu hot_real=%llu hot_unknown=%llu hot_fake=%llu\n",
		   vhuge_stats.damon_scan_last_checked,
		   vhuge_stats.damon_scan_last_transhuge,
		   vhuge_stats.damon_scan_last_hot_mapped,
		   vhuge_stats.damon_scan_last_hot_real,
		   vhuge_stats.damon_scan_last_hot_unknown,
		   vhuge_stats.damon_scan_last_hot_fake);
	seq_printf(m, "work: unknown_probe_in_flight=%d migration_in_flight=%d migration_max_per_scan=%u\n",
		   atomic_read(&vhuge_probe_inflight),
		   atomic_read(&vhuge_migrate_inflight),
		   READ_ONCE(vhuge_damon_migrate_max_per_scan));
#else
	seq_puts(m, "damon: available=0\n");
#endif
	if (verbose) {
		seq_printf(m, "activity: marked=%llu mark_fail=%llu captured=%llu consumed=%llu released=%llu alloc_empty=%llu\n",
			   vhuge_stats.mark_success, vhuge_stats.mark_fail,
			   vhuge_stats.captured, vhuge_stats.consumed,
			   vhuge_stats.released, vhuge_stats.alloc_empty);
		seq_printf(m, "alloc: local=%llu remote=%llu local_miss=%llu\n",
			   vhuge_stats.alloc_local,
			   vhuge_stats.alloc_remote,
			   vhuge_stats.alloc_local_miss);
		seq_printf(m, "thp: fault_allocs=%llu khugepaged_allocs=%llu daemon_bypass=%llu tool_bypass=%llu\n",
			   vhuge_stats.thp_fault_allocs,
			   vhuge_stats.khugepaged_allocs,
			   vhuge_stats.daemon_pool_bypass,
			   vhuge_stats.tool_pool_bypass);
		seq_printf(m, "validation_debug: checked=%llu failed=%llu returned=%llu released=%llu quarantined=%llu\n",
			   vhuge_stats.validation_checked,
			   vhuge_stats.validation_failed,
			   vhuge_stats.validation_returned,
			   vhuge_stats.validation_released,
			   vhuge_stats.validation_quarantined);
#ifdef CONFIG_VHUGE_DAMON
		seq_printf(m, "damon_policy_debug: refill_failures=%llu recovered=%llu fill_to_ignored=%llu started=%llu stopped=%llu last_status_flags=0x%x last_status_seq=%u\n",
			   vhuge_stats.damon_policy_refill_failures,
			   vhuge_stats.damon_policy_refill_recovered,
			   vhuge_stats.damon_policy_fill_to_ignored,
			   vhuge_stats.damon_policy_started,
			   vhuge_stats.damon_policy_stopped,
			   READ_ONCE(vhuge_daemon_status.flags),
			   READ_ONCE(vhuge_daemon_status.last_refill_seq));
		seq_printf(m, "damon_targets: registered=%llu reaped=%llu rebuilds=%llu scout_started=%llu scout_recovered=%llu alloc_hot=%llu cold_bypass=%llu reserve_bypass=%llu\n",
			   vhuge_stats.damon_targets_registered,
			   vhuge_stats.damon_targets_reaped,
			   vhuge_stats.damon_target_rebuilds,
			   vhuge_stats.damon_scout_started,
			   vhuge_stats.damon_scout_recovered,
			   vhuge_stats.damon_alloc_hot,
			   vhuge_stats.damon_alloc_cold_bypass,
			   vhuge_stats.damon_alloc_reserve_bypass);
		seq_printf(m, "fixed_budget_debug: armed=%llu activated=%llu reserved=%llu released=%llu exhausted=%llu\n",
			   vhuge_stats.damon_fixed_armed,
			   vhuge_stats.damon_fixed_activated,
			   vhuge_stats.damon_budget_reserved,
			   vhuge_stats.damon_budget_released,
			   vhuge_stats.damon_budget_exhausted);
		seq_printf(m, "classification_total: checked=%llu transhuge=%llu hot_mapped=%llu hot_real=%llu hot_unknown=%llu hot_fake=%llu skipped_no_hot=%llu\n",
			   vhuge_stats.damon_scan_slots_checked,
			   vhuge_stats.damon_scan_transhuge,
			   vhuge_stats.damon_scan_hot_mapped,
			   vhuge_stats.damon_scan_hot_real,
			   vhuge_stats.damon_scan_hot_unknown,
			   vhuge_stats.damon_scan_hot_fake,
			   vhuge_stats.damon_scan_skipped_no_hot);
		seq_printf(m, "unknown_probe: threshold_cycles=%u samples=%u last_median_cycles=%llu max_median_cycles=%llu max_per_scan=%u cooldown_sec=%u in_flight=%d queued=%llu attempted=%llu real=%llu fake=%llu failed=%llu faulted=%llu cooldown_skip=%llu inflight_skip=%llu budget_hit=%llu last_us=%llu max_us=%llu\n",
			   READ_ONCE(vhuge_damon_probe_threshold_cycles),
			   READ_ONCE(vhuge_damon_probe_samples),
			   vhuge_stats.damon_probe_last_median_cycles,
			   vhuge_stats.damon_probe_max_median_cycles,
			   READ_ONCE(vhuge_damon_probe_max_per_scan),
			   READ_ONCE(vhuge_damon_probe_cooldown_sec),
			   atomic_read(&vhuge_probe_inflight),
			   vhuge_stats.damon_probe_queued,
			   vhuge_stats.damon_probe_attempted,
			   vhuge_stats.damon_probe_real,
			   vhuge_stats.damon_probe_fake,
			   vhuge_stats.damon_probe_failed,
			   vhuge_stats.damon_probe_faulted,
			   vhuge_stats.damon_probe_cooldown_skip,
			   vhuge_stats.damon_probe_inflight_skip,
			   vhuge_stats.damon_probe_budget_hit,
			   vhuge_stats.damon_probe_last_us,
			   vhuge_stats.damon_probe_max_us);
		seq_printf(m, "migration: max_per_scan=%u in_flight=%d queued=%llu attempted=%llu succeeded=%llu failed=%llu no_pool=%llu revalidate_failed=%llu isolate_failed=%llu migrate_pages_failed=%llu account_failed=%llu limit_hit=%llu last_total_us=%llu max_total_us=%llu\n",
			   READ_ONCE(vhuge_damon_migrate_max_per_scan),
			   atomic_read(&vhuge_migrate_inflight),
			   vhuge_stats.damon_migrate_queued,
			   vhuge_stats.damon_migrate_attempted,
			   vhuge_stats.damon_migrate_succeeded,
			   vhuge_stats.damon_migrate_failed,
			   vhuge_stats.damon_migrate_no_pool,
			   vhuge_stats.damon_migrate_revalidate_failed,
			   vhuge_stats.damon_migrate_isolate_failed,
			   vhuge_stats.damon_migrate_pages_failed,
			   vhuge_stats.damon_migrate_account_failed,
			   vhuge_stats.damon_migrate_limit_hit,
			   vhuge_stats.damon_migrate_last_total_us,
			   vhuge_stats.damon_migrate_max_total_us);
		seq_printf(m, "migration_debug: last_isolate_us=%llu max_isolate_us=%llu last_pool_us=%llu max_pool_us=%llu last_migrate_us=%llu max_migrate_us=%llu last_account_us=%llu max_account_us=%llu\n",
			   vhuge_stats.damon_migrate_last_isolate_us,
			   vhuge_stats.damon_migrate_max_isolate_us,
			   vhuge_stats.damon_migrate_last_pool_us,
			   vhuge_stats.damon_migrate_max_pool_us,
			   vhuge_stats.damon_migrate_last_migrate_us,
			   vhuge_stats.damon_migrate_max_migrate_us,
			   vhuge_stats.damon_migrate_last_account_us,
			   vhuge_stats.damon_migrate_max_account_us);
		seq_printf(m, "damon_debug: aggregations=%llu table_full=%llu last_update_us=%llu max_update_us=%llu classify_interval_sec=%u candidate_max=%u passes=%llu last_classify_us=%llu max_classify_us=%llu\n",
			   vhuge_stats.damon_aggregations,
			   vhuge_stats.damon_hot_table_full,
			   vhuge_stats.damon_last_update_us,
			   vhuge_stats.damon_max_update_us,
			   VHUGE_DAMON_CLASSIFY_INTERVAL_SEC,
			   READ_ONCE(vhuge_damon_hot_pmd_max),
			   vhuge_stats.damon_scan_passes,
			   vhuge_stats.damon_last_scan_us,
			   vhuge_stats.damon_max_scan_us);
#endif
	}
	seq_printf(m, "ownership: process_real=%llu system_pool=%u owners=%u lost=%llu splits=%llu\n\n",
		   vhuge_stats.owned_current, READ_ONCE(vhuge_pool_count),
		   row_count, vhuge_stats.ownership_lost,
		   vhuge_stats.ownership_splits);

	seq_printf(m, "%-8s %-16s %10s %10s %10s %10s %10s\n",
		   "PID", "COMM", "REAL_HPS", "FAULT", "KHUGEPGD",
		   "PROBED", "MIGRATED");
	seq_printf(m, "%-8s %-16s %10u %10s %10s %10s %10s\n",
		   "-", "system-pool", READ_ONCE(vhuge_pool_count), "-",
		   "-", "-", "-");
	for (i = 0; i < row_count; i++) {
		seq_printf(m, "%-8d %-16s %10u %10llu %10llu %10llu %10llu\n",
			   rows[i].tgid, rows[i].comm, rows[i].real_hps,
			   rows[i].thp_fault_hps, rows[i].khugepaged_hps,
			   rows[i].damon_probe_hps, rows[i].migrated_hps);
	}
	kfree(rows);
	return 0;
}

static ssize_t enabled_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_enabled) ? 1 : 0);
}

static ssize_t enabled_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	bool val;
	int ret = kstrtobool(buf, &val);

	if (ret)
		return ret;
#ifdef CONFIG_VHUGE_DAMON
	mutex_lock(&vhuge_damon_lock);
#endif
	if (!val) {
		mutex_lock(&vhuge_enable_lock);
		WRITE_ONCE(vhuge_enabled, false);
		WRITE_ONCE(vhuge_pool_fill_to, 0);
		mutex_unlock(&vhuge_enable_lock);
#ifdef CONFIG_VHUGE_DAMON
		vhuge_damon_policy_reset_locked(true);
		vhuge_damon_budget_clear();
		WRITE_ONCE(vhuge_damon_policy_mode,
			   VHUGE_DAMON_POLICY_AUTO);
		vhuge_monitor_clear();
#endif
		mutex_lock(&vhuge_enable_lock);
		vhuge_clear_marks();
		vhuge_clear_consumed();
		vhuge_clear_ownership();
		vhuge_drain_pool();
		mutex_unlock(&vhuge_enable_lock);
	} else {
		mutex_lock(&vhuge_enable_lock);
		WRITE_ONCE(vhuge_enabled, true);
		mutex_unlock(&vhuge_enable_lock);
#ifdef CONFIG_VHUGE_DAMON
		vhuge_damon_policy_reset_locked(false);
#endif
	}
#ifdef CONFIG_VHUGE_DAMON
	mutex_unlock(&vhuge_damon_lock);
#endif
	pr_info("vhuge: enabled set to %u by pid=%d comm=%s\n",
		val ? 1 : 0, task_pid_nr(current), current->comm);
	return count;
}

static struct kobj_attribute enabled_attr = __ATTR_RW(enabled);

static int parse_u32_range(const char *buf, u32 min, u32 max, u32 *out)
{
	u32 val;
	int ret = kstrtou32(buf, 0, &val);

	if (ret)
		return ret;
	if (val < min || val > max)
		return -EINVAL;
	*out = val;
	return 0;
}

static ssize_t proc_verbosity_show(struct kobject *kobj,
				   struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_proc_verbosity));
}

static ssize_t proc_verbosity_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 1, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_proc_verbosity, val);
	return count;
}

static struct kobj_attribute proc_verbosity_attr =
	__ATTR_RW(proc_verbosity);

static ssize_t pool_current_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_pool_count);
}

/* __ATTR() would expand the name "current" to the kernel macro. */
static struct kobj_attribute current_attr = {
	.attr = { .name = "current", .mode = 0444 },
	.show = pool_current_show,
};

static ssize_t target_show(struct kobject *kobj, struct kobj_attribute *attr,
			   char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_pool_target);
}

static ssize_t target_store(struct kobject *kobj, struct kobj_attribute *attr,
			    const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, UINT_MAX, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_pool_target, val);
	if (READ_ONCE(vhuge_pool_low) > val)
		WRITE_ONCE(vhuge_pool_low, val);
	if (READ_ONCE(vhuge_pool_max) < val)
		WRITE_ONCE(vhuge_pool_max, val);
	return count;
}

static struct kobj_attribute target_attr = __ATTR_RW(target);

static ssize_t low_watermark_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_pool_low);
}

static ssize_t low_watermark_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, READ_ONCE(vhuge_pool_target), &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_pool_low, val);
	return count;
}

static struct kobj_attribute low_watermark_attr = __ATTR_RW(low_watermark);

static ssize_t fill_to_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_pool_fill_to));
}

static ssize_t fill_to_store(struct kobject *kobj, struct kobj_attribute *attr,
			     const char *buf, size_t count)
{
	u32 max = READ_ONCE(vhuge_pool_max);
	u32 val;
	int ret = parse_u32_range(buf, 0, max, &val);

	if (ret)
		return ret;
	if (val && val >= max)
		return -EINVAL;
	WRITE_ONCE(vhuge_pool_fill_to, val);
	return count;
}

static struct kobj_attribute fill_to_attr = __ATTR_RW(fill_to);

static ssize_t max_show(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_pool_max);
}

static ssize_t max_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, READ_ONCE(vhuge_pool_target),
				  UINT_MAX, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_pool_max, val);
	if (READ_ONCE(vhuge_pool_fill_to) >= val)
		WRITE_ONCE(vhuge_pool_fill_to, val ? val - 1 : 0);
	return count;
}

static struct kobj_attribute max_attr = __ATTR_RW(max);

#ifdef CONFIG_VHUGE_DAMON
static int parse_ulong_range(const char *buf, unsigned long min,
			     unsigned long max, unsigned long *out)
{
	unsigned long val;
	int ret = kstrtoul(buf, 0, &val);

	if (ret)
		return ret;
	if (val < min || val > max)
		return -EINVAL;
	*out = val;
	return 0;
}

static ssize_t damon_enabled_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_damon_enabled) ? 1 : 0);
}

static ssize_t damon_enabled_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	bool val;
	int ret = kstrtobool(buf, &val);

	if (ret)
		return ret;
	mutex_lock(&vhuge_damon_lock);
	WRITE_ONCE(vhuge_damon_enabled, val);
	if (READ_ONCE(vhuge_damon_policy_mode) ==
	    VHUGE_DAMON_POLICY_FIXED_BUDGET) {
		if (val) {
			ret = vhuge_damon_start_locked();
			if (ret == -ESRCH)
				ret = 0;
		} else {
			vhuge_damon_stop_locked();
		}
	} else {
		vhuge_damon_policy_reset_locked(true);
	}
	mutex_unlock(&vhuge_damon_lock);
	return ret ? ret : count;
}

static struct kobj_attribute damon_enabled_attr = __ATTR_RW(damon_enabled);

static ssize_t damon_policy_mode_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_damon_policy_mode));
}

static ssize_t damon_policy_mode_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, VHUGE_DAMON_POLICY_AUTO,
				  VHUGE_DAMON_POLICY_FIXED_BUDGET, &val);

	if (ret)
		return ret;
	mutex_lock(&vhuge_damon_lock);
	if (val == VHUGE_DAMON_POLICY_FIXED_BUDGET)
		ret = vhuge_damon_fixed_arm_locked();
	else
		ret = vhuge_damon_auto_mode_locked();
	mutex_unlock(&vhuge_damon_lock);
	return ret ? ret : count;
}

static struct kobj_attribute damon_policy_mode_attr =
	__ATTR_RW(damon_policy_mode);

static ssize_t damon_budget_fcfs_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", READ_ONCE(vhuge_damon_budget_fcfs));
}

static ssize_t damon_budget_fcfs_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	bool val;
	int ret = kstrtobool(buf, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_budget_fcfs, val);
	return count;
}

static struct kobj_attribute damon_budget_fcfs_attr =
	__ATTR_RW(damon_budget_fcfs);

static ssize_t damon_hot_budget_pages_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(vhuge_damon_hot_budget_pages));
}

static ssize_t damon_hot_budget_pages_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	unsigned long flags;
	u32 val;
	int ret = parse_u32_range(buf, 0, UINT_MAX, &val);

	if (ret)
		return ret;
	spin_lock_irqsave(&vhuge_damon_budget_lock, flags);
	if (val < vhuge_damon_hot_budget_in_use) {
		spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
		return -EBUSY;
	}
	vhuge_damon_hot_budget_pages = val;
	spin_unlock_irqrestore(&vhuge_damon_budget_lock, flags);
	return count;
}

static struct kobj_attribute damon_hot_budget_pages_attr =
	__ATTR_RW(damon_hot_budget_pages);

static ssize_t damon_fixed_scout_aggregations_show(
		struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(vhuge_damon_fixed_scout_aggrs));
}

static ssize_t damon_fixed_scout_aggregations_store(
		struct kobject *kobj, struct kobj_attribute *attr,
		const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 1, 1000, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_fixed_scout_aggrs, val);
	return count;
}

static struct kobj_attribute damon_fixed_scout_aggregations_attr =
	__ATTR_RW(damon_fixed_scout_aggregations);

static ssize_t damon_sample_us_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(vhuge_damon_sample_us));
}

static ssize_t damon_sample_us_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long val;
	int ret = parse_ulong_range(buf, 1000UL, 60000000UL, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_sample_us, val);
	if (READ_ONCE(vhuge_damon_aggr_us) < val)
		WRITE_ONCE(vhuge_damon_aggr_us, val);
	ret = vhuge_damon_restart_if_running();
	return ret ? ret : count;
}

static struct kobj_attribute damon_sample_us_attr =
	__ATTR_RW(damon_sample_us);

static ssize_t damon_aggr_us_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(vhuge_damon_aggr_us));
}

static ssize_t damon_aggr_us_store(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   const char *buf, size_t count)
{
	unsigned long val;
	int ret = parse_ulong_range(buf, READ_ONCE(vhuge_damon_sample_us),
				    60000000UL, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_aggr_us, val);
	if (READ_ONCE(vhuge_damon_update_us) < val)
		WRITE_ONCE(vhuge_damon_update_us, val);
	ret = vhuge_damon_restart_if_running();
	return ret ? ret : count;
}

static struct kobj_attribute damon_aggr_us_attr = __ATTR_RW(damon_aggr_us);

static ssize_t damon_update_us_show(struct kobject *kobj,
				    struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(vhuge_damon_update_us));
}

static ssize_t damon_update_us_store(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     const char *buf, size_t count)
{
	unsigned long val;
	int ret = parse_ulong_range(buf, READ_ONCE(vhuge_damon_aggr_us),
				    600000000UL, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_update_us, val);
	ret = vhuge_damon_restart_if_running();
	return ret ? ret : count;
}

static struct kobj_attribute damon_update_us_attr =
	__ATTR_RW(damon_update_us);

static ssize_t damon_min_regions_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(vhuge_damon_min_regions));
}

static ssize_t damon_min_regions_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long val;
	int ret = parse_ulong_range(buf, 3UL,
				    READ_ONCE(vhuge_damon_max_regions),
				    &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_min_regions, val);
	ret = vhuge_damon_restart_if_running();
	return ret ? ret : count;
}

static struct kobj_attribute damon_min_regions_attr =
	__ATTR_RW(damon_min_regions);

static ssize_t damon_max_regions_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%lu\n", READ_ONCE(vhuge_damon_max_regions));
}

static ssize_t damon_max_regions_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	unsigned long val;
	int ret = parse_ulong_range(buf, READ_ONCE(vhuge_damon_min_regions),
				    100000UL, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_max_regions, val);
	ret = vhuge_damon_restart_if_running();
	return ret ? ret : count;
}

static struct kobj_attribute damon_max_regions_attr =
	__ATTR_RW(damon_max_regions);

static ssize_t damon_hot_threshold_bp_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_hot_threshold_bp);
}

static ssize_t damon_hot_threshold_bp_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 10000, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_hot_threshold_bp, val);
	return count;
}

static struct kobj_attribute damon_hot_threshold_bp_attr =
	__ATTR_RW(damon_hot_threshold_bp);

static ssize_t damon_hot_pmd_max_show(struct kobject *kobj,
				      struct kobj_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_hot_pmd_max);
}

static ssize_t damon_hot_pmd_max_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 1, VHUGE_DAMON_HOT_PMD_MAX_DEF,
				  &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_hot_pmd_max, val);
	return count;
}

static struct kobj_attribute damon_hot_pmd_max_attr =
	__ATTR_RW(damon_hot_pmd_max);

static ssize_t damon_migration_reserve_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_migration_reserve);
}

static ssize_t damon_migration_reserve_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, VHUGE_POOL_MAX_DEF, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_migration_reserve, val);
	return count;
}

static struct kobj_attribute damon_migration_reserve_attr =
	__ATTR_RW(damon_migration_reserve);

static ssize_t damon_probe_threshold_cycles_show(struct kobject *kobj,
						 struct kobj_attribute *attr,
						 char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_probe_threshold_cycles);
}

static ssize_t damon_probe_threshold_cycles_store(struct kobject *kobj,
						  struct kobj_attribute *attr,
						  const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 1, 10000, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_probe_threshold_cycles, val);
	vhuge_damon_probe_calibrated = false;
	return count;
}

static struct kobj_attribute damon_probe_threshold_cycles_attr =
	__ATTR_RW(damon_probe_threshold_cycles);

static ssize_t damon_probe_self_calibrate_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "%u hit=%u miss=%u\n",
			  vhuge_damon_probe_self_calibrate,
			  vhuge_damon_probe_calib_hit,
			  vhuge_damon_probe_calib_miss);
}

static ssize_t damon_probe_self_calibrate_store(struct kobject *kobj,
						struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	if (kstrtobool(buf, &vhuge_damon_probe_self_calibrate))
		return -EINVAL;
	vhuge_damon_probe_calibrated = false;
	return count;
}

static struct kobj_attribute damon_probe_self_calibrate_attr =
	__ATTR_RW(damon_probe_self_calibrate);

static ssize_t damon_probe_samples_show(struct kobject *kobj,
					struct kobj_attribute *attr,
					char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_probe_samples);
}

static ssize_t damon_probe_samples_store(struct kobject *kobj,
					 struct kobj_attribute *attr,
					 const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 1, VHUGE_DAMON_PROBE_SAMPLES_MAX,
				  &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_probe_samples, val);
	return count;
}

static struct kobj_attribute damon_probe_samples_attr =
	__ATTR_RW(damon_probe_samples);

static ssize_t damon_probe_max_per_scan_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_probe_max_per_scan);
}

static ssize_t damon_probe_max_per_scan_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 1024, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_probe_max_per_scan, val);
	return count;
}

static struct kobj_attribute damon_probe_max_per_scan_attr =
	__ATTR_RW(damon_probe_max_per_scan);

static ssize_t damon_probe_cooldown_sec_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_probe_cooldown_sec);
}

static ssize_t damon_probe_cooldown_sec_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 3600, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_probe_cooldown_sec, val);
	return count;
}

static struct kobj_attribute damon_probe_cooldown_sec_attr =
	__ATTR_RW(damon_probe_cooldown_sec);

static ssize_t damon_migrate_max_per_scan_show(struct kobject *kobj,
					       struct kobj_attribute *attr,
					       char *buf)
{
	return sysfs_emit(buf, "%u\n", vhuge_damon_migrate_max_per_scan);
}

static ssize_t damon_migrate_max_per_scan_store(struct kobject *kobj,
						struct kobj_attribute *attr,
						const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 1024, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_migrate_max_per_scan, val);
	return count;
}

static struct kobj_attribute damon_migrate_max_per_scan_attr =
	__ATTR_RW(damon_migrate_max_per_scan);

static ssize_t damon_pressure_failures_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(vhuge_damon_pressure_failures));
}

static ssize_t damon_pressure_failures_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 1, 1024, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_pressure_failures, val);
	mutex_lock(&vhuge_damon_lock);
	if (READ_ONCE(vhuge_damon_policy_mode) == VHUGE_DAMON_POLICY_AUTO)
		vhuge_damon_policy_reset_locked(true);
	mutex_unlock(&vhuge_damon_lock);
	return count;
}

static struct kobj_attribute damon_pressure_failures_attr =
	__ATTR_RW(damon_pressure_failures);

static ssize_t damon_stop_watermark_pct_show(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(vhuge_damon_stop_watermark_pct));
}

static ssize_t damon_stop_watermark_pct_store(struct kobject *kobj,
					      struct kobj_attribute *attr,
					      const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 100, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_stop_watermark_pct, val);
	return count;
}

static struct kobj_attribute damon_stop_watermark_pct_attr =
	__ATTR_RW(damon_stop_watermark_pct);

static ssize_t damon_min_runtime_sec_show(struct kobject *kobj,
					  struct kobj_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%u\n",
			  READ_ONCE(vhuge_damon_min_runtime_sec));
}

static ssize_t damon_min_runtime_sec_store(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   const char *buf, size_t count)
{
	u32 val;
	int ret = parse_u32_range(buf, 0, 3600, &val);

	if (ret)
		return ret;
	WRITE_ONCE(vhuge_damon_min_runtime_sec, val);
	return count;
}

static struct kobj_attribute damon_min_runtime_sec_attr =
	__ATTR_RW(damon_min_runtime_sec);

static ssize_t damon_policy_reset_store(struct kobject *kobj,
					struct kobj_attribute *attr,
					const char *buf, size_t count)
{
	bool val;
	int ret = kstrtobool(buf, &val);

	if (ret)
		return ret;
	if (val) {
		mutex_lock(&vhuge_damon_lock);
		if (READ_ONCE(vhuge_damon_policy_mode) ==
		    VHUGE_DAMON_POLICY_FIXED_BUDGET)
			ret = vhuge_damon_fixed_arm_locked();
		else
			vhuge_damon_policy_reset_locked(true);
		mutex_unlock(&vhuge_damon_lock);
	}
	return ret ? ret : count;
}

static struct kobj_attribute damon_policy_reset_attr =
	__ATTR(damon_policy_reset, 0200, NULL, damon_policy_reset_store);
#endif

static ssize_t reset_stats_store(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 const char *buf, size_t count)
{
	struct vhuge_mm_owner *owner;
	unsigned long flags;
	bool val;
	int ret = kstrtobool(buf, &val);

	if (ret)
		return ret;
	if (val) {
		memset(&vhuge_stats, 0, sizeof(vhuge_stats));
		spin_lock_irqsave(&vhuge_owner_lock, flags);
		list_for_each_entry(owner, &vhuge_owner_list, list)
			vhuge_stats.owned_current += owner->real_hps;
		spin_unlock_irqrestore(&vhuge_owner_lock, flags);
	}
	return count;
}

static struct kobj_attribute reset_stats_attr =
	__ATTR(reset_stats, 0200, NULL, reset_stats_store);

static struct attribute *vhuge_attrs[] = {
	&enabled_attr.attr,
	&proc_verbosity_attr.attr,
	&current_attr.attr,
	&target_attr.attr,
	&low_watermark_attr.attr,
	&fill_to_attr.attr,
	&max_attr.attr,
#ifdef CONFIG_VHUGE_DAMON
	&damon_enabled_attr.attr,
	&damon_policy_mode_attr.attr,
	&damon_hot_budget_pages_attr.attr,
	&damon_budget_fcfs_attr.attr,
	&damon_fixed_scout_aggregations_attr.attr,
	&damon_sample_us_attr.attr,
	&damon_aggr_us_attr.attr,
	&damon_update_us_attr.attr,
	&damon_min_regions_attr.attr,
	&damon_max_regions_attr.attr,
	&damon_hot_threshold_bp_attr.attr,
	&damon_hot_pmd_max_attr.attr,
	&damon_migration_reserve_attr.attr,
	&damon_probe_threshold_cycles_attr.attr,
	&damon_probe_self_calibrate_attr.attr,
	&damon_probe_samples_attr.attr,
	&damon_probe_max_per_scan_attr.attr,
	&damon_probe_cooldown_sec_attr.attr,
	&damon_migrate_max_per_scan_attr.attr,
	&damon_pressure_failures_attr.attr,
	&damon_stop_watermark_pct_attr.attr,
	&damon_min_runtime_sec_attr.attr,
	&damon_policy_reset_attr.attr,
#endif
	&reset_stats_attr.attr,
	NULL,
};
ATTRIBUTE_GROUPS(vhuge);

static int __init vhuge_init(void)
{
	int ret;

	WRITE_ONCE(vhuge_pool_low, vhuge_boot_low_watermark());
	pr_info("vhuge: boot low watermark=%u PMD THPs (%u%% of free RAM)\n",
		READ_ONCE(vhuge_pool_low), VHUGE_POOL_LOW_PCT_DEF);

#ifdef CONFIG_VHUGE_DAMON
	vhuge_damon_wq = alloc_workqueue("vhuge_damon", WQ_UNBOUND, 0);
	if (!vhuge_damon_wq)
		return -ENOMEM;
	ret = vhuge_probe_worker_start();
	if (ret)
		goto err_wq;
	ret = vhuge_migrate_worker_start();
	if (ret)
		goto err_probe_worker;
#endif

	vhuge_proc = proc_create_single(VHUGE_PROC_NAME, 0444, NULL,
					vhuge_proc_show);
	if (!vhuge_proc) {
		ret = -ENOMEM;
		goto err_workers;
	}

	vhuge_kobj = kobject_create_and_add(VHUGE_SYSFS_NAME, kernel_kobj);
	if (!vhuge_kobj) {
		ret = -ENOMEM;
		goto err_proc;
	}
	ret = sysfs_create_groups(vhuge_kobj, vhuge_groups);
	if (ret)
		goto err_kobj;

	ret = misc_register(&vhuge_miscdev);
	if (ret)
		goto err_sysfs;

	pr_info("vhuge: loaded; dev=/dev/%s sysfs=/sys/kernel/%s proc=/proc/%s\n",
		VHUGE_DEV_NAME, VHUGE_SYSFS_NAME, VHUGE_PROC_NAME);
	return 0;

err_sysfs:
	sysfs_remove_groups(vhuge_kobj, vhuge_groups);
err_kobj:
	kobject_put(vhuge_kobj);
	vhuge_kobj = NULL;
err_proc:
	proc_remove(vhuge_proc);
err_workers:
#ifdef CONFIG_VHUGE_DAMON
	vhuge_migrate_worker_stop();
err_probe_worker:
	vhuge_probe_worker_stop();
err_wq:
	destroy_workqueue(vhuge_damon_wq);
	vhuge_damon_wq = NULL;
#endif
	return ret;
}

module_init(vhuge_init);

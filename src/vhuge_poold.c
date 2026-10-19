#include "../include/common.h"
#include "../include/mem.h"
#include "../include/pref.h"
#include "../include/utils.h"
#include <ptedit_header.h>

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>

#define VHUGE_DEV_PATH "/dev/vhuge_pool"
#define VHUGE_SYSFS_DIR "/sys/kernel/vhuge_pool"
#define TLB_THRESHOLD_DEFAULT 55
#define TLB_SAMPLES_DEFAULT 5
#define TLB_THRESHOLD_MAX 10000U
#define TLB_SAMPLES_MAX 32U
#define THRESHOLD_CALIB_SAMPLES 2000U
#define CHECK_MS_DEFAULT 1000U
#define VALIDATE_MS_DEFAULT 10000U
#define VALIDATE_BATCH_MAX 512U
#define BATCH_HPS_DEFAULT 256U
#define PREF_SETUP_PAGES 64U
#define MAX_CPU_LIST 4096U
#define VHUGE_MARK_ADDRS_MAX 4096U
#define VHUGE_IOC_MAGIC 'V'

struct vhuge_mark_addrs {
    u64 addrs;
    u32 nr_addrs;
    u32 flags;
};

struct vhuge_validate_batch {
    u32 max_entries;
    u32 nr_entries;
    u32 flags;
};

struct vhuge_validate_result {
    u32 slot;
    u32 is_real;
};

struct vhuge_validate_report {
    u64 results;
    u32 nr_results;
    u32 flags;
};

struct vhuge_daemon_status {
    u32 next_refill_check_sec;
    u32 next_validation_sec;
    u32 last_refill_wanted;
    u32 last_refill_captured;
    u32 last_refill_success_pct;
    u32 last_validation_batch;
    u32 flags;
    u32 last_refill_seq;
};

#define VHUGE_DAEMON_STATUS_REFILL_ATTEMPTED (1U << 0)
#define VHUGE_DAEMON_STATUS_REFILL_FILL_TO   (1U << 1)
#define VHUGE_DAMON_POLICY_FIXED_BUDGET      1U

#define VHUGE_IOC_MARK_REAL_ADDRS \
    _IOW(VHUGE_IOC_MAGIC, 1, struct vhuge_mark_addrs)
#define VHUGE_IOC_GET_VALIDATE_BATCH \
    _IOWR(VHUGE_IOC_MAGIC, 2, struct vhuge_validate_batch)
#define VHUGE_IOC_REPORT_VALIDATE \
    _IOW(VHUGE_IOC_MAGIC, 3, struct vhuge_validate_report)
#define VHUGE_IOC_SET_DAEMON_STATUS \
    _IOW(VHUGE_IOC_MAGIC, 4, struct vhuge_daemon_status)

typedef struct {
    u8 **pages;
    bool *is_real;
    const u32 *cpus;
    u32 start_idx;
    u32 end_idx;
    u32 samples_per_page;
    u32 tlb_threshold;
    u32 worker_id;
    i32 err;
    u64 probe_us;
} TlbWorkerCtx;

typedef struct {
    u32 target;
    u32 low_watermark;
    u32 max;
    u32 batch_hps;
    u32 check_ms;
    u32 validate_ms;
    u32 samples_per_page;
    u32 core_limit;
    u32 threshold;
    bool threshold_manual;
    bool tlb_only;
    bool once;
    bool log_validation_timing;
} PooldConfig;

typedef struct {
    u32 last_refill_wanted;
    u32 last_refill_captured;
    u32 last_refill_success_pct;
    u32 last_validation_batch;
    u32 last_refill_flags;
    u32 last_refill_seq;
} PooldState;

static void log_msg(FILE *stream, const char *level, const char *fmt, ...)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    va_list ap;

    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(stream, "%s %s", ts, level);
    va_start(ap, fmt);
    vfprintf(stream, fmt, ap);
    va_end(ap);
    fflush(stream);
}

#define LOG_INFO(...) log_msg(stdout, INFO, __VA_ARGS__)
#define LOG_WARN(...) log_msg(stderr, WRN, __VA_ARGS__)
#define LOG_ERR(...)  log_msg(stderr, ERR, __VA_ARGS__)

typedef struct {
    u8 **pages;
    u8 **eligible;
    bool *is_real;
    pthread_t *threads;
    TlbWorkerCtx *workers;
    u32 request;
    u32 eligible_count;
    u32 workers_started;
    u32 marked;
    u64 real;
    u64 not_real;
    u64 alloc_failed;
    u64 not_promoted;
    u64 probe_us;
    u64 worker_probe_us;
} CandidateBatch;

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static bool parse_u32_arg(const char *name, const char *s, u32 *out)
{
    char *end = NULL;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 0);
    while (end && *end && isspace((unsigned char)*end))
        end++;
    if (errno || !end || *end || v > UINT32_MAX) {
        LOG_ERR("invalid %s: %s\n", name, s);
        return false;
    }
    *out = (u32)v;
    return true;
}

static bool parse_nonzero_u32_arg(const char *name, const char *s, u32 *out)
{
    return parse_u32_arg(name, s, out) && *out;
}

static int read_u32_file(const char *path, u32 *out)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -errno;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return n < 0 ? -errno : -EIO;
    buf[n] = '\0';
    return parse_u32_arg(path, buf, out) ? 0 : -EINVAL;
}

static int read_sysfs_u32(const char *name, u32 *out)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", VHUGE_SYSFS_DIR, name);
    return read_u32_file(path, out);
}

static int write_u32_file(const char *path, u32 val)
{
    char buf[64];
    int fd;
    int len;
    ssize_t n;

    len = snprintf(buf, sizeof(buf), "%u\n", val);
    if (len < 0 || (size_t)len >= sizeof(buf))
        return -EINVAL;

    fd = open(path, O_WRONLY);
    if (fd < 0)
        return -errno;
    n = write(fd, buf, (size_t)len);
    close(fd);
    if (n != len)
        return n < 0 ? -errno : -EIO;
    return 0;
}

static int write_sysfs_u32(const char *name, u32 val)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/%s", VHUGE_SYSFS_DIR, name);
    return write_u32_file(path, val);
}

static int publish_probe_config(const PooldConfig *cfg)
{
    int ret;

    ret = write_sysfs_u32("damon_probe_threshold_cycles", cfg->threshold);
    if (ret && ret != -ENOENT)
        return ret;
    ret = write_sysfs_u32("damon_probe_samples", cfg->samples_per_page);
    if (ret && ret != -ENOENT)
        return ret;
    return 0;
}

static int read_sysfs_knobs(PooldConfig *cfg)
{
    return read_sysfs_u32("target", &cfg->target) ||
           read_sysfs_u32("low_watermark", &cfg->low_watermark) ||
           read_sysfs_u32("max", &cfg->max) ? -1 : 0;
}

static int read_pool_current(u32 *current)
{
    return read_sysfs_u32("current", current);
}

static void clear_fill_to(void)
{
    int ret = write_sysfs_u32("fill_to", 0);

    if (ret)
        LOG_WARN("vhuge-poold: failed to clear reached fill_to: %s\n",
                 strerror(-ret));
}

static int read_damon_policy_mode(u32 *mode)
{
    int ret = read_sysfs_u32("damon_policy_mode", mode);

    if (ret == -ENOENT) {
        *mode = 0;
        return 0;
    }
    return ret;
}

static u64 monotonic_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000ULL + (u64)ts.tv_nsec / 1000000ULL;
}

static void sleep_until_ms(u64 deadline_ms)
{
    struct timespec ts;

    if (stop_requested)
        return;
    ts.tv_sec = (time_t)(deadline_ms / 1000ULL);
    ts.tv_nsec = (long)((deadline_ms % 1000ULL) * 1000000ULL);
    while (!stop_requested && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
                                              &ts, NULL) == EINTR)
        ;
}

static u32 seconds_until(u64 now_ms, u64 deadline_ms)
{
    if (deadline_ms <= now_ms)
        return 0;
    return (u32)((deadline_ms - now_ms + 999ULL) / 1000ULL);
}

static void publish_daemon_status(int fd, const PooldState *state,
                                  u64 next_refill_ms, u64 next_validate_ms)
{
    u64 now = monotonic_ms();
    struct vhuge_daemon_status status = {
        .next_refill_check_sec = seconds_until(now, next_refill_ms),
        .next_validation_sec = seconds_until(now, next_validate_ms),
        .last_refill_wanted = state->last_refill_wanted,
        .last_refill_captured = state->last_refill_captured,
        .last_refill_success_pct = state->last_refill_success_pct,
        .last_validation_batch = state->last_validation_batch,
        .flags = state->last_refill_flags,
        .last_refill_seq = state->last_refill_seq,
    };

    (void)ioctl(fd, VHUGE_IOC_SET_DAEMON_STATUS, &status);
}

static bool cpu_is_primary_online(u32 cpu)
{
    char path[128];
    char buf[128];
    int fd;
    ssize_t n;
    u32 first;

    if (cpu == 0)
        return true;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/online", cpu);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    if (strtoul(buf, NULL, 0) != 1)
        return false;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return true;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return true;
    buf[n] = '\0';
    first = (u32)strtoul(buf, NULL, 10);
    return first == cpu;
}

static u32 discover_physical_cpus(u32 *cpus, u32 max_cpus)
{
    long nconf = get_nprocs_conf();
    u32 count = 0;

    if (nconf <= 0)
        nconf = get_nprocs();
    if (nconf <= 0)
        nconf = 1;

    for (u32 cpu = 0; cpu < (u32)nconf && count < max_cpus; cpu++) {
        if (!cpu_is_primary_online(cpu))
            continue;
        cpus[count++] = cpu;
    }

    if (!count)
        cpus[count++] = 0;
    return count;
}

static bool is_thp_promoted(u8 *addr)
{
    ptedit_entry_t vm = ptedit_resolve(addr, 0);

    if (!(vm.valid & PTEDIT_VALID_MASK_PMD))
        return false;
    return ptedit_cast(vm.pmd, ptedit_pmd_t).size == 1;
}

static u32 tlb_probe_threshold_cycles(u32 *miss, u32 *hit)
{
    u8 *hp = mmap_anon_thp_init(NULL, HUGE_PAGE_SIZE, 0x5A);
    u64 *miss_samples = calloc(THRESHOLD_CALIB_SAMPLES, sizeof(*miss_samples));
    u64 *hit_samples = calloc(THRESHOLD_CALIB_SAMPLES, sizeof(*hit_samples));
    u64 median_miss = 0;
    u64 median_hit = 0;
    u32 threshold = TLB_THRESHOLD_DEFAULT;

    if (!hp || !miss_samples || !hit_samples) {
        LOG_WARN("TLB threshold calibration setup failed; using default %u cycles\n",
                 TLB_THRESHOLD_DEFAULT);
        goto out;
    }
    if (!is_thp_promoted(hp)) {
        LOG_WARN("Calibration THP was not PMD-promoted; using default %u cycles\n",
                 TLB_THRESHOLD_DEFAULT);
        goto out;
    }

    _mfence();
    for (u32 s = 0; s < THRESHOLD_CALIB_SAMPLES; s++) {
        maccess(hp);
        _lfence();
        ptedit_invalidate_tlb(hp);
        _mfence();
        _lfence();
        miss_samples[s] = time_maccess(hp);
        _mfence();
    }

    for (u32 s = 0; s < THRESHOLD_CALIB_SAMPLES; s++) {
        maccess(hp);
        _lfence();
        hit_samples[s] = time_maccess(hp);
        _mfence();
    }

    median_miss = calc_median_u64(miss_samples, THRESHOLD_CALIB_SAMPLES);
    median_hit = calc_median_u64(hit_samples, THRESHOLD_CALIB_SAMPLES);
    *miss = (u32)median_miss;
    *hit = (u32)median_hit;

    if (median_hit && median_miss > median_hit)
        threshold = (u32)((2 * median_hit + 3 * median_miss) / 5);
    else
        LOG_WARN("Unusable TLB calibration medians (miss=%llu hit=%llu); using default %u cycles\n",
                 (ull)median_miss, (ull)median_hit, TLB_THRESHOLD_DEFAULT);

out:
    if (hp)
        munmap(hp, HUGE_PAGE_SIZE);
    free(miss_samples);
    free(hit_samples);
    return threshold;
}

static void candidate_batch_destroy(CandidateBatch *batch)
{
    for (u32 i = 0; batch->pages && i < batch->request; i++)
        if (batch->pages[i])
            munmap(batch->pages[i], HUGE_PAGE_SIZE);
    free(batch->workers);
    free(batch->threads);
    free(batch->is_real);
    free(batch->eligible);
    free(batch->pages);
}

static int candidate_batch_init(CandidateBatch *batch, u32 request)
{
    memset(batch, 0, sizeof(*batch));
    batch->request = request;
    batch->pages = calloc(request, sizeof(*batch->pages));
    batch->eligible = calloc(request, sizeof(*batch->eligible));
    batch->is_real = calloc(request, sizeof(*batch->is_real));
    return batch->pages && batch->eligible && batch->is_real ? 0 : -ENOMEM;
}

typedef struct {
    CandidateBatch *batch;
    const u32 *cpus;
    u32 worker_id;
    u32 start_idx;
    u32 end_idx;
    u64 alloc_failed;
    int err;
} AllocWorkerCtx;

static void *alloc_worker(void *_ctx)
{
    AllocWorkerCtx *ctx = (AllocWorkerCtx *)_ctx;
    u32 cpu = ctx->cpus[ctx->worker_id];

    if (set_cpu_affinity((u16)cpu) != 0) {
        ctx->err = -1;
        return NULL;
    }

    for (u32 i = ctx->start_idx; i < ctx->end_idx && !stop_requested; i++) {
        ctx->batch->pages[i] = mmap_anon_thp_init(NULL, HUGE_PAGE_SIZE, 0x5A);
        if (!ctx->batch->pages[i])
            ctx->alloc_failed++;
    }

    return NULL;
}

/* Set when the prefetcher method found a reliable configuration. */
static PrefConfig pref_cfg;
static bool use_pref;

static void *tlb_worker(void *_ctx)
{
    TlbWorkerCtx *ctx = (TlbWorkerCtx *)_ctx;
    u64 *samples = NULL;
    u32 cpu = ctx->cpus[ctx->worker_id];

    if (set_cpu_affinity((u16)cpu) != 0) {
        ctx->err = -1;
        return NULL;
    }

    if (!use_pref) {
        samples = calloc(ctx->samples_per_page, sizeof(*samples));
        if (!samples) {
            ctx->err = -1;
            return NULL;
        }
    }

    ctx->probe_us = 0;
    for (u32 i = ctx->start_idx; i < ctx->end_idx; i++) {
        u8 *hp = ctx->pages[i];
        u8 *hp_mid = hp + (1 << 20);
        u64 begin = time_us();

        if (use_pref) {
            ctx->is_real[i] = pref_is_real(&pref_cfg, hp, ctx->samples_per_page);
            ctx->probe_us += time_us() - begin;
            continue;
        }
        for (u32 s = 0; s < ctx->samples_per_page; s++) {
            maccess(hp);
            _lfence();
            ptedit_invalidate_tlb(hp);
            _mfence();
            maccess(hp_mid);
            _mfence();
            _lfence();
            samples[s] = time_maccess(hp);
        }
        ctx->probe_us += time_us() - begin;
        ctx->is_real[i] = calc_median_u64(samples, ctx->samples_per_page) <
                          ctx->tlb_threshold;
    }

    free(samples);
    return NULL;
}

static int alloc_candidate_pages(CandidateBatch *batch, const u32 *cpus,
                                 u32 cpu_count)
{
    pthread_t *threads;
    AllocWorkerCtx *workers;
    u32 worker_count = min(batch->request, cpu_count);
    u32 base, rem, start = 0, workers_started = 0;
    int ret = -1;

    if (!batch->request)
        return 0;

    threads = calloc(worker_count, sizeof(*threads));
    workers = calloc(worker_count, sizeof(*workers));
    if (!threads || !workers)
        goto out;

    base = batch->request / worker_count;
    rem = batch->request % worker_count;
    for (u32 t = 0; t < worker_count; t++) {
        u32 count = base + (t < rem ? 1 : 0);
        AllocWorkerCtx *ctx = &workers[t];

        *ctx = (AllocWorkerCtx) {
            .batch = batch,
            .cpus = cpus,
            .worker_id = t,
            .start_idx = start,
            .end_idx = start + count,
        };
        start += count;

        if (pthread_create(&threads[t], NULL, alloc_worker, ctx)) {
            LOG_ERR("vhuge-poold: failed to create allocation worker %u\n", t);
            goto join;
        }
        workers_started++;
    }

join:
    for (u32 t = 0; t < workers_started; t++)
        pthread_join(threads[t], NULL);
    if (workers_started != worker_count)
        goto out;

    for (u32 t = 0; t < workers_started; t++) {
        if (workers[t].err)
            goto out;
        batch->alloc_failed += workers[t].alloc_failed;
    }

    for (u32 i = 0; i < batch->request; i++) {
        if (!batch->pages[i])
            continue;
        if (!is_thp_promoted(batch->pages[i])) {
            batch->not_promoted++;
            continue;
        }
        batch->eligible[batch->eligible_count++] = batch->pages[i];
    }

    ret = 0;

out:
    free(workers);
    free(threads);
    return ret;
}

static int probe_candidate_pages(CandidateBatch *batch, const PooldConfig *cfg,
                                 const u32 *cpus, u32 cpu_count)
{
    u32 worker_count = min(batch->eligible_count, cpu_count);
    u32 base, rem;
    u32 start = 0;
    u64 begin;

    if (!batch->eligible_count)
        return 0;

    batch->threads = calloc(worker_count, sizeof(*batch->threads));
    batch->workers = calloc(worker_count, sizeof(*batch->workers));
    if (!batch->threads || !batch->workers)
        return -ENOMEM;

    base = batch->eligible_count / worker_count;
    rem = batch->eligible_count % worker_count;
    begin = time_us();

    for (u32 t = 0; t < worker_count; t++) {
        u32 count = base + (t < rem ? 1 : 0);
        TlbWorkerCtx *ctx = &batch->workers[t];

        *ctx = (TlbWorkerCtx) {
            .pages = batch->eligible,
            .is_real = batch->is_real,
            .cpus = cpus,
            .start_idx = start,
            .end_idx = start + count,
            .samples_per_page = cfg->samples_per_page,
            .tlb_threshold = cfg->threshold,
            .worker_id = t,
        };
        start += count;

        if (pthread_create(&batch->threads[t], NULL, tlb_worker, ctx)) {
            LOG_ERR("vhuge-poold: failed to create worker %u\n", t);
            for (u32 j = 0; j < batch->workers_started; j++)
                pthread_join(batch->threads[j], NULL);
            return -1;
        }
        batch->workers_started++;
    }

    for (u32 t = 0; t < batch->workers_started; t++)
        pthread_join(batch->threads[t], NULL);
    batch->probe_us = time_us() - begin;

    for (u32 t = 0; t < batch->workers_started; t++) {
        if (batch->workers[t].err)
            return -1;
        batch->worker_probe_us += batch->workers[t].probe_us;
    }

    for (u32 i = 0; i < batch->eligible_count; i++) {
        if (batch->is_real[i])
            batch->real++;
        else
            batch->not_real++;
    }

    return 0;
}

static int mark_real_addrs(int fd, u8 **pages, bool *is_real, u32 count,
                           u32 *marked)
{
    u64 addrs[VHUGE_MARK_ADDRS_MAX];
    struct vhuge_mark_addrs req;
    u32 nr = 0;

    for (u32 i = 0; i < count; i++) {
        if (!is_real[i])
            continue;
        addrs[nr++] = (u64)(uintptr_t)pages[i];
    }
    *marked = nr;
    if (!nr)
        return 0;

    req.addrs = (u64)(uintptr_t)addrs;
    req.nr_addrs = nr;
    req.flags = 0;
    return ioctl(fd, VHUGE_IOC_MARK_REAL_ADDRS, &req);
}

static int fill_pool_once(int fd, const PooldConfig *cfg, const u32 *cpus,
                          u32 cpu_count, u32 request_hps, u32 *captured)
{
    u32 request = min(cfg->batch_hps, request_hps);
    CandidateBatch batch;
    u32 before = 0, after = 0;
    int ret = -1;

    *captured = 0;
    (void)read_pool_current(&before);
    if (candidate_batch_init(&batch, request))
        goto out;

    if (alloc_candidate_pages(&batch, cpus, cpu_count))
        goto out;

    if (!batch.eligible_count) {
        LOG_INFO("vhuge-poold: requested=%u eligible=0 alloc_failed=%llu not_promoted=%llu\n",
                 request, (ull)batch.alloc_failed, (ull)batch.not_promoted);
        ret = 0;
        goto out;
    }

    if (probe_candidate_pages(&batch, cfg, cpus, cpu_count))
        goto out;

    if (mark_real_addrs(fd, batch.eligible, batch.is_real,
                        batch.eligible_count, &batch.marked) != 0) {
        LOG_ERR("vhuge-poold: ioctl mark failed: %s\n", strerror(errno));
        goto out;
    }

    LOG_INFO("vhuge-poold: probed=%u real=%llu not_real=%llu marked=%u alloc_failed=%llu not_promoted=%llu probe_ms=%.3f worker_probe_ms=%.3f\n",
             batch.eligible_count, (ull)batch.real, (ull)batch.not_real,
             batch.marked, (ull)batch.alloc_failed, (ull)batch.not_promoted,
             batch.probe_us / 1e3, batch.worker_probe_us / 1e3);
    ret = 0;

out:
    candidate_batch_destroy(&batch);
    if (!ret && read_pool_current(&after) == 0 && after > before)
        *captured = after - before;
    return ret;
}

static int probe_validation_pages(u8 **pages, bool *is_real, u32 count,
                                  const PooldConfig *cfg, const u32 *cpus,
                                  u32 cpu_count)
{
    pthread_t *threads;
    TlbWorkerCtx *workers;
    u32 worker_count = min(count, cpu_count);
    u32 base, rem, start = 0, workers_started = 0;
    int ret = -1;

    if (!count)
        return 0;
    threads = calloc(worker_count, sizeof(*threads));
    workers = calloc(worker_count, sizeof(*workers));
    if (!threads || !workers)
        goto out;

    base = count / worker_count;
    rem = count % worker_count;
    for (u32 t = 0; t < worker_count; t++) {
        u32 n = base + (t < rem ? 1 : 0);
        TlbWorkerCtx *ctx = &workers[t];

        *ctx = (TlbWorkerCtx) {
            .pages = pages,
            .is_real = is_real,
            .cpus = cpus,
            .start_idx = start,
            .end_idx = start + n,
            .samples_per_page = cfg->samples_per_page,
            .tlb_threshold = cfg->threshold,
            .worker_id = t,
        };
        start += n;
        if (pthread_create(&threads[t], NULL, tlb_worker, ctx)) {
            LOG_ERR("vhuge-poold: failed to create validation worker %u\n", t);
            goto join;
        }
        workers_started++;
    }

join:
    for (u32 t = 0; t < workers_started; t++)
        pthread_join(threads[t], NULL);
    if (workers_started != worker_count)
        goto out;
    for (u32 t = 0; t < workers_started; t++)
        if (workers[t].err)
            goto out;
    ret = 0;

out:
    free(workers);
    free(threads);
    return ret;
}

static int validate_pool_once(int fd, const PooldConfig *cfg, const u32 *cpus,
                              u32 cpu_count, u32 pool_current,
                              u32 *batch_done)
{
    struct vhuge_validate_batch batch = {
        .max_entries = min(VALIDATE_BATCH_MAX, pool_current),
    };
    struct vhuge_validate_result *results = NULL;
    struct vhuge_validate_report report;
    u8 **pages = NULL;
    bool *is_real = NULL;
    u8 *mapping = MAP_FAILED;
    size_t len;
    u32 real = 0, not_real = 0;
    u64 wall_begin = time_us();
    u64 probe_begin = 0, probe_us = 0;
    bool have_probe_results = false;
    int ret = -1;

    *batch_done = 0;
    if (!batch.max_entries)
        return 0;
    if (ioctl(fd, VHUGE_IOC_GET_VALIDATE_BATCH, &batch) != 0) {
        if (errno == EBUSY)
            return 0;
        LOG_ERR("vhuge-poold: validation batch ioctl failed: %s\n",
                strerror(errno));
        return -1;
    }
    if (!batch.nr_entries)
        return 0;

    len = (size_t)batch.nr_entries * HUGE_PAGE_SIZE;
    mapping = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping == MAP_FAILED) {
        LOG_ERR("vhuge-poold: validation mmap failed: %s\n",
                strerror(errno));
        goto out_report_failed;
    }

    pages = calloc(batch.nr_entries, sizeof(*pages));
    is_real = calloc(batch.nr_entries, sizeof(*is_real));
    results = calloc(batch.nr_entries, sizeof(*results));
    if (!pages || !is_real || !results)
        goto out_unmap;

    for (u32 i = 0; i < batch.nr_entries; i++)
        pages[i] = mapping + (size_t)i * HUGE_PAGE_SIZE;
    probe_begin = time_us();
    if (probe_validation_pages(pages, is_real, batch.nr_entries, cfg,
                               cpus, cpu_count) != 0)
        goto out_unmap;
    probe_us = time_us() - probe_begin;

    for (u32 i = 0; i < batch.nr_entries; i++) {
        results[i].slot = i;
        results[i].is_real = is_real[i] ? 1 : 0;
        if (is_real[i])
            real++;
        else
            not_real++;
    }
    have_probe_results = true;
    ret = 0;

out_unmap:
    if (mapping != MAP_FAILED)
        munmap(mapping, len);
out_report_failed:
    if (!have_probe_results) {
        if (!results) {
            results = calloc(batch.nr_entries, sizeof(*results));
            if (!results)
                goto out_free;
        }
        for (u32 i = 0; i < batch.nr_entries; i++) {
            results[i].slot = i;
            results[i].is_real = 0;
        }
    }
    report = (struct vhuge_validate_report) {
        .results = (u64)(uintptr_t)results,
        .nr_results = batch.nr_entries,
    };
    if (ioctl(fd, VHUGE_IOC_REPORT_VALIDATE, &report) != 0) {
        LOG_ERR("vhuge-poold: validation report ioctl failed: %s\n",
                strerror(errno));
        ret = -1;
    } else if (!have_probe_results) {
        LOG_WARN("vhuge-poold: validation failed before probing; released %u pages\n",
                 batch.nr_entries);
        ret = 0;
    } else if (cfg->log_validation_timing) {
        LOG_INFO("vhuge-poold: validation_batch=%u real=%u not_real=%u wall_ms=%.3f probe_ms=%.3f\n",
                 batch.nr_entries, real, not_real,
                 (time_us() - wall_begin) / 1e3, probe_us / 1e3);
    }
    *batch_done = batch.nr_entries;

out_free:
    free(results);
    free(is_real);
    free(pages);
    return ret;
}

static bool tlb_probe_one(u8 *hp, const PooldConfig *cfg)
{
    u64 samples[TLB_SAMPLES_MAX];
    u32 n = min(cfg->samples_per_page, (u32)TLB_SAMPLES_MAX);

    for (u32 s = 0; s < n; s++) {
        maccess(hp);
        _lfence();
        ptedit_invalidate_tlb(hp);
        _mfence();
        maccess(hp + (1 << 20));
        _mfence();
        _lfence();
        samples[s] = time_maccess(hp);
    }
    return calc_median_u64(samples, n) < cfg->threshold;
}

/*
 * Try the prefetcher method: find one real and one fake huge page with the
 * TLB method, then search for a configuration that tells them apart. If there
 * is none, the daemon keeps using the TLB method.
 */
static void pref_setup(const PooldConfig *cfg)
{
    u8 *pages[PREF_SETUP_PAGES] = {0};
    u8 *real = NULL, *fake = NULL;
    u64 start = time_us();

    for (u32 i = 0; i < PREF_SETUP_PAGES && (!real || !fake); i++) {
        pages[i] = mmap_anon_thp_init(NULL, HUGE_PAGE_SIZE, 0x5A);
        if (!pages[i] || !is_thp_promoted(pages[i]))
            continue;
        if (tlb_probe_one(pages[i], cfg))
            real = real ? real : pages[i];
        else
            fake = fake ? fake : pages[i];
    }

    if (!real || !fake)
        LOG_INFO("prefetcher method: found only %s huge pages; using the TLB method\n",
                 real ? "real" : "fake");
    else if (!pref_find_config(real, fake, &pref_cfg))
        LOG_INFO("prefetcher method: no reliable configuration on this processor (%.1f ms); using the TLB method\n",
                 (time_us() - start) / 1e3);
    else {
        use_pref = true;
        LOG_INFO("prefetcher method: %u accesses %uB apart, probe line %uB beyond, difference %u%% (%.1f ms)\n",
                 pref_cfg.n_train, pref_cfg.stride,
                 pref_cfg.n_jump * pref_cfg.stride, pref_cfg.diff_pct,
                 (time_us() - start) / 1e3);
    }

    for (u32 i = 0; i < PREF_SETUP_PAGES; i++)
        if (pages[i])
            munmap(pages[i], HUGE_PAGE_SIZE);
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n"
           "  -S, --samples N          TLB samples per page [default: %u]\n"
           "  -c, --num-core N         Max physical cores to use [default: all]\n"
           "      --batch-hps N        Max THPs to probe per refill [default: %u]\n"
           "      --check-ms N         Pool check interval [default: %u]\n"
           "      --validate-ms N      Validation interval [default: %u]\n"
           "      --threshold N        Use fixed TLB threshold instead of calibration\n"
           "      --tlb-only           Do not try the prefetcher method\n"
           "      --log-validation-timing\n"
           "                            Log validation batch wall/probe time\n"
           "      --once               Refill once if current is below low watermark\n"
           "  -h, --help               Show this help\n",
           prog, TLB_SAMPLES_DEFAULT, BATCH_HPS_DEFAULT, CHECK_MS_DEFAULT,
           VALIDATE_MS_DEFAULT);
}

int main(int argc, char **argv)
{
    PooldConfig cfg = {
        .batch_hps = BATCH_HPS_DEFAULT,
        .check_ms = CHECK_MS_DEFAULT,
        .validate_ms = VALIDATE_MS_DEFAULT,
        .samples_per_page = TLB_SAMPLES_DEFAULT,
    };
    PooldState state = {0};
    u32 cpus[MAX_CPU_LIST];
    u32 physical_cpus;
    u32 miss = 0, hit = 0;
    int fd = -1;
    int opt;
    int ret = EXIT_FAILURE;

    static const struct option opts[] = {
        {"help", no_argument, 0, 'h'},
        {"samples", required_argument, 0, 'S'},
        {"num-core", required_argument, 0, 'c'},
        {"batch-hps", required_argument, 0, 1000},
        {"check-ms", required_argument, 0, 1001},
        {"threshold", required_argument, 0, 1002},
        {"once", no_argument, 0, 1003},
        {"validate-ms", required_argument, 0, 1004},
        {"log-validation-timing", no_argument, 0, 1005},
        {"tlb-only", no_argument, 0, 1006},
        {0, 0, 0, 0},
    };

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while ((opt = getopt_long(argc, argv, "hS:c:", opts, NULL)) != -1) {
        switch (opt) {
        case 'h':
            usage(argv[0]);
            return EXIT_SUCCESS;
        case 'S':
            if (!parse_nonzero_u32_arg("--samples", optarg, &cfg.samples_per_page))
                return EXIT_FAILURE;
            if (cfg.samples_per_page > TLB_SAMPLES_MAX) {
                LOG_ERR("--samples must be <= %u so the kernel probe can use the same value\n",
                        TLB_SAMPLES_MAX);
                return EXIT_FAILURE;
            }
            break;
        case 'c':
            if (!parse_u32_arg("--num-core", optarg, &cfg.core_limit))
                return EXIT_FAILURE;
            break;
        case 1000:
            if (!parse_nonzero_u32_arg("--batch-hps", optarg, &cfg.batch_hps))
                return EXIT_FAILURE;
            if (cfg.batch_hps > VHUGE_MARK_ADDRS_MAX) {
                LOG_ERR("--batch-hps must be <= %u\n", VHUGE_MARK_ADDRS_MAX);
                return EXIT_FAILURE;
            }
            break;
        case 1001:
            if (!parse_nonzero_u32_arg("--check-ms", optarg, &cfg.check_ms))
                return EXIT_FAILURE;
            break;
        case 1002:
            if (!parse_nonzero_u32_arg("--threshold", optarg, &cfg.threshold))
                return EXIT_FAILURE;
            if (cfg.threshold > TLB_THRESHOLD_MAX) {
                LOG_ERR("--threshold must be <= %u so the kernel probe can use the same value\n",
                        TLB_THRESHOLD_MAX);
                return EXIT_FAILURE;
            }
            cfg.threshold_manual = true;
            break;
        case 1003:
            cfg.once = true;
            break;
        case 1004:
            if (!parse_nonzero_u32_arg("--validate-ms", optarg, &cfg.validate_ms))
                return EXIT_FAILURE;
            break;
        case 1005:
            cfg.log_validation_timing = true;
            break;
        case 1006:
            cfg.tlb_only = true;
            break;
        default:
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (read_sysfs_knobs(&cfg) != 0) {
        LOG_ERR("failed to read %s knobs\n", VHUGE_SYSFS_DIR);
        return EXIT_FAILURE;
    }

    physical_cpus = discover_physical_cpus(cpus, MAX_CPU_LIST);
    if (cfg.core_limit && cfg.core_limit < physical_cpus)
        physical_cpus = cfg.core_limit;

    if (ptedit_init()) {
        LOG_ERR("ptedit_init failed; is PTEditor loaded?\n");
        return EXIT_FAILURE;
    }

    if (!cfg.threshold_manual)
        cfg.threshold = tlb_probe_threshold_cycles(&miss, &hit);
    if (cfg.threshold > TLB_THRESHOLD_MAX) {
        LOG_ERR("calibrated threshold %u exceeds kernel probe limit %u\n",
                cfg.threshold, TLB_THRESHOLD_MAX);
        goto out_ptedit;
    }

    if (cfg.threshold_manual) {
        LOG_INFO("vhuge-poold: target=%u low=%u max=%u batch=%u check_ms=%u validate_ms=%u physical_cores=%u samples=%u threshold=%u\n",
                 cfg.target, cfg.low_watermark, cfg.max, cfg.batch_hps,
                 cfg.check_ms, cfg.validate_ms, physical_cpus,
                 cfg.samples_per_page, cfg.threshold);
    } else {
        LOG_INFO("vhuge-poold: target=%u low=%u max=%u batch=%u check_ms=%u validate_ms=%u physical_cores=%u samples=%u threshold=%u miss=%u hit=%u calib_samples=%u\n",
                 cfg.target, cfg.low_watermark, cfg.max, cfg.batch_hps,
                 cfg.check_ms, cfg.validate_ms, physical_cpus,
                 cfg.samples_per_page, cfg.threshold, miss, hit,
                 THRESHOLD_CALIB_SAMPLES);
    }
    if (publish_probe_config(&cfg) != 0) {
        LOG_ERR("failed to publish DAMON probe config to %s\n",
                VHUGE_SYSFS_DIR);
        goto out_ptedit;
    }

    fd = open(VHUGE_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG_ERR("failed to open %s: %s\n", VHUGE_DEV_PATH, strerror(errno));
        goto out_ptedit;
    }
    /* After the open, so that the setup pages do not come from the pool. */
    if (!cfg.tlb_only)
        pref_setup(&cfg);

    u64 now = monotonic_ms();
    u64 next_refill_ms = now;
    u64 next_validate_ms = now + cfg.validate_ms;

    publish_daemon_status(fd, &state, next_refill_ms, next_validate_ms);

    while (!stop_requested) {
        u32 current = 0;
        bool have_current = false;
        u64 wake_ms;

        now = monotonic_ms();
        if (now >= next_refill_ms) {
            u32 fill_to = 0;
            u32 policy_mode = 0;
            u32 desired = cfg.target;
            bool should_refill;

            if (read_pool_current(&current) != 0) {
                LOG_ERR("failed to read pool current\n");
                goto out_close;
            }
            have_current = true;
            if (read_sysfs_u32("fill_to", &fill_to) != 0) {
                LOG_ERR("failed to read fill_to\n");
                goto out_close;
            }
            if (read_damon_policy_mode(&policy_mode) != 0) {
                LOG_ERR("failed to read DAMON policy mode\n");
                goto out_close;
            }
            if (fill_to && current >= fill_to) {
                clear_fill_to();
                fill_to = 0;
            }
            if (fill_to) {
                desired = fill_to;
                should_refill = current < fill_to;
            } else
                should_refill =
                    policy_mode != VHUGE_DAMON_POLICY_FIXED_BUDGET &&
                    current < cfg.low_watermark;
            if (should_refill) {
                u32 needed = desired > current ? desired - current : 0;
                u32 request_hps = fill_to ? cfg.batch_hps : needed;
                u32 attempted = min(cfg.batch_hps, request_hps);
                u32 captured = 0;
                u32 pct = 100;
                u32 sleep_ms = cfg.check_ms;

                /*
                 * fill_to is an operator-directed prefill target. Keep each
                 * pass at full batch size until the target is reached; tiny
                 * tail requests make convergence unnecessarily slow when only
                 * a fraction of probed THPs are real.
                 */
                if (needed && fill_pool_once(fd, &cfg, cpus, physical_cpus,
                                             request_hps, &captured) != 0)
                    goto out_close;
                if (attempted)
                    pct = (u32)(((u64)captured * 100ULL) / attempted);
                if (!fill_to && needed && pct < 25U)
                    sleep_ms = pct > 10U ? 5000U : 10000U;
                if (fill_to && needed) {
                    u32 post_current;

                    if (read_pool_current(&post_current) == 0) {
                        current = post_current;
                        if (current >= fill_to)
                            clear_fill_to();
                    } else {
                        LOG_WARN("vhuge-poold: failed to re-read pool current after refill\n");
                    }
                }
                state.last_refill_wanted = attempted;
                state.last_refill_captured = captured;
                state.last_refill_success_pct = pct;
                state.last_refill_flags =
                    VHUGE_DAEMON_STATUS_REFILL_ATTEMPTED |
                    (fill_to ? VHUGE_DAEMON_STATUS_REFILL_FILL_TO : 0U);
                state.last_refill_seq++;
                if (!state.last_refill_seq)
                    state.last_refill_seq = 1;
                next_refill_ms = monotonic_ms() + sleep_ms;
            } else {
                next_refill_ms = now + cfg.check_ms;
            }
        }

        now = monotonic_ms();
        if (!cfg.once && now >= next_validate_ms) {
            u32 batch_done = 0;

            if (!have_current && read_pool_current(&current) != 0) {
                LOG_ERR("failed to read pool current\n");
                goto out_close;
            }
            if (validate_pool_once(fd, &cfg, cpus, physical_cpus, current,
                                   &batch_done) != 0)
                goto out_close;
            state.last_validation_batch = batch_done;
            next_validate_ms = monotonic_ms() + cfg.validate_ms;
        }

        if (cfg.once)
            break;
        publish_daemon_status(fd, &state, next_refill_ms, next_validate_ms);
        now = monotonic_ms();
        wake_ms = next_refill_ms < next_validate_ms ? next_refill_ms : next_validate_ms;
        if (wake_ms > now)
            sleep_until_ms(wake_ms);
    }

    ret = EXIT_SUCCESS;
    if (stop_requested)
        LOG_INFO("vhuge-poold: stopping\n");

out_close:
    close(fd);
out_ptedit:
    ptedit_cleanup();
    return ret;
}

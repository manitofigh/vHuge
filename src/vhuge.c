#include "../include/common.h"
#include "../include/utils.h"
#include "../include/mem.h"
#include "../include/pref.h"
#include <ptedit_header.h>
#include "../include/gpa_hpa.h"
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

enum {
    OPT_HP_KIND = 1000,
    OPT_APPEND_RESULTS,
};

typedef enum {
    HP_KIND_THP = 0,
    HP_KIND_HUGETLB,
} HpKind;

#define TLB_SAMPLES_DEFAULT 5
#define TLB_THRESHOLD_DEFAULT 55

// compare against the host's backing through misc/host/gpa_hpa_km (-d)
static bool debug;

typedef struct {
    u8** pages;
    u64* medians;
    bool* is_real;
    u32 start_idx;
    u32 end_idx;
    u32 samples_per_page;
    u32 tlb_threshold;
    i32 core_id;
    i32 err;
    u64 probe_us;
} TlbProbeWorkerCtx;

// the idea is to get a feel of the latency when there's data cache hit + TLB miss,
// followed by data cache hit + TLB hit, and then take a (somewhat) middle spot
// in between both cases and use that as the threshold
static u32 tlb_probe_threshold_cycles(HpKind hp_kind, u32* miss, u32* hit)
{
    const u32 n_samples_tlb_miss = 50000, n_samples_tlb_hit = 50000;
    u8* hp = NULL;
    u64* samples_tlb_miss = calloc(n_samples_tlb_miss, sizeof(*samples_tlb_miss));
    u64* samples_tlb_hit = calloc(n_samples_tlb_hit, sizeof(*samples_tlb_hit));
    u64 median_tlb_miss = 0;
    u64 median_tlb_hit = 0;
    u32 threshold = TLB_THRESHOLD_DEFAULT;

    if (hp_kind == HP_KIND_HUGETLB)
        hp = mmap_huge_private_init(NULL, HUGE_PAGE_SIZE, 'a');
    else
        hp = mmap_anon_thp_init(NULL, HUGE_PAGE_SIZE, 0x5A);

    if (!hp || !samples_tlb_miss || !samples_tlb_hit) {
        fprintf(stderr, WRN "TLB threshold calibration setup failed; using default %u cycles\n",
                TLB_THRESHOLD_DEFAULT);
        goto out;
    }

    _mfence();
    for (u32 s = 0; s < n_samples_tlb_miss; s++) {
        maccess(hp); // cache data
        _lfence();
        ptedit_invalidate_tlb(hp);
        _mfence();
        samples_tlb_miss[s] = time_maccess(hp);
        _mfence();
    }

    for (u32 s = 0; s < n_samples_tlb_hit; s++) {
        maccess(hp); // cache data
        _lfence();
        samples_tlb_hit[s] = time_maccess(hp);
        _mfence();
    }

    median_tlb_miss = calc_median_u64(samples_tlb_miss, n_samples_tlb_miss);
    *miss = median_tlb_miss;
    median_tlb_hit = calc_median_u64(samples_tlb_hit, n_samples_tlb_hit);
    *hit = median_tlb_hit;
    if (median_tlb_hit == 0 || median_tlb_miss == 0 || median_tlb_miss <= median_tlb_hit) {
        fprintf(stderr, WRN "TLB threshold calibration returned unusable medians (miss: %llu, hit: %llu); using default %u cycles\n",
                (ull)median_tlb_miss, (ull)median_tlb_hit, TLB_THRESHOLD_DEFAULT);
        goto out;
    }
    threshold = (u32)((2 * median_tlb_hit + 3 * median_tlb_miss) / 5);
    // similar to:
    // https://github.com/zzrcxb/LLCFeasible/blob/e2d9d80959324071a19f0feafaa4b02794cac277/include/cache/latency.h#L30

out:
    if (hp)
        munmap(hp, HUGE_PAGE_SIZE);
    free(samples_tlb_miss);
    free(samples_tlb_hit);
    return threshold;
}

static bool append_tlb_results(const char* path, HpKind hp_kind, u32 requested_pages,
                               u64 probed_pages, u64 real_pages, u64 fake_pages,
                               u64 alloc_failed, u32 threshold, u32 tlb_miss,
                               u32 tlb_hit, u32 samples_per_page, u32 core_limit,
                               u64 elapsed_us)
{
    static const char* header =
        "timestamp_unix\ttimestamp_utc\thostname\thp_kind\trequested_pages\tprobed_pages\treal_pages\tfake_pages\treal_pct\tfake_pct\talloc_failed\tthreshold_cycles\ttlb_miss_median\ttlb_hit_median\tsamples_per_page\tcore_limit\telapsed_us\n";
    char timestamp[32];
    char hostname[256];
    struct stat st;
    struct tm tm;
    time_t now;
    FILE* fp;
    f64 real_pct = probed_pages ? ((f64)real_pages * 100.0) / (f64)probed_pages : 0.0;
    f64 fake_pct = probed_pages ? ((f64)fake_pages * 100.0) / (f64)probed_pages : 0.0;
    bool write_header;

    if (!path)
        return true;

    fp = fopen(path, "a");
    if (!fp) {
        fprintf(stderr, ERR "Failed to open results file %s: %s\n", path, strerror(errno));
        return false;
    }

    write_header = fstat(fileno(fp), &st) == 0 && st.st_size == 0;
    now = time(NULL);
    if (!gmtime_r(&now, &tm) || strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0)
        snprintf(timestamp, sizeof(timestamp), "unknown");
    if (gethostname(hostname, sizeof(hostname)) != 0)
        snprintf(hostname, sizeof(hostname), "unknown");
    hostname[sizeof(hostname) - 1] = '\0';

    if ((write_header && fputs(header, fp) == EOF) ||
        fprintf(fp, "%lld\t%s\t%s\t%s\t%u\t%llu\t%llu\t%llu\t%.6f\t%.6f\t%llu\t%u\t%u\t%u\t%u\t%u\t%llu\n",
                (long long)now, timestamp, hostname,
                hp_kind == HP_KIND_HUGETLB ? "hugetlb" : "thp",
                requested_pages, (ull)probed_pages, (ull)real_pages,
                (ull)fake_pages, real_pct, fake_pct, (ull)alloc_failed,
                threshold, tlb_miss, tlb_hit, samples_per_page, core_limit,
                (ull)elapsed_us) < 0 || fclose(fp) != 0) {
        fprintf(stderr, ERR "Failed to append results file %s: %s\n", path, strerror(errno));
        return false;
    }

    return true;
}

static bool parse_u32_opt(const char* name, const char* arg, u32* out)
{
    char* end = NULL;
    ull val;

    errno = 0;
    val = strtoull(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || val > UINT32_MAX) {
        fprintf(stderr, ERR "Invalid value for %s: %s\n", name, arg);
        return false;
    }

    *out = (u32)val;
    return true;
}

static bool parse_hp_kind(const char* arg, HpKind* out)
{
    if (strcmp(arg, "thp") == 0) {
        *out = HP_KIND_THP;
        return true;
    }
    if (strcmp(arg, "hugetlb") == 0) {
        *out = HP_KIND_HUGETLB;
        return true;
    }
    return false;
}

static bool is_thp_promoted(u8* addr)
{
    ptedit_entry_t vm = ptedit_resolve(addr, 0);
    if (!(vm.valid & PTEDIT_VALID_MASK_PMD))
        return false;
    return ptedit_cast(vm.pmd, ptedit_pmd_t).size == 1;
}

static void* tlb_probe_worker(void* _ctx)
{
    TlbProbeWorkerCtx* ctx = (TlbProbeWorkerCtx*)_ctx;
    u64* samples = NULL;

    if (set_cpu_affinity((u16)ctx->core_id) != 0) {
        ctx->err = -1;
        return NULL;
    }

    samples = calloc(ctx->samples_per_page, sizeof(*samples));
    if (!samples) {
        ctx->err = -1;
        return NULL;
    }

    ctx->probe_us = 0;
    for (u32 i = ctx->start_idx; i < ctx->end_idx; i++) {
        u8* hp = ctx->pages[i];
        u8* hp_mid = hp + (1 << 20);

        u64 core_probe_s = 0;
        u64 core_probe_e = 0;
        _mfence();
        core_probe_s = time_us();
        for (u32 s = 0; s < ctx->samples_per_page; s++) {
            maccess(hp);
            _lfence();
            ptedit_invalidate_tlb(hp);
            _mfence();
            maccess(hp_mid);
            _mfence(); _lfence();
            samples[s] = time_maccess(hp);
        }
        core_probe_e = time_us();
        ctx->probe_us += core_probe_e - core_probe_s;

        ctx->medians[i] = calc_median_u64(samples, ctx->samples_per_page);
        ctx->is_real[i] = (ctx->medians[i] < ctx->tlb_threshold);
    }

    free(samples);
    return NULL;
}

// Prefetcher method: search for a configuration using one real and one fake
// huge page found by the TLB method. If none is reliable on this processor,
// the TLB results stand.
static void run_pref_stage(u8** pages, const bool* tlb_is_real, u32 n, u32 rounds)
{
    PrefConfig cfg;
    u8* real = NULL;
    u8* fake = NULL;
    u64 start;
    u32 agree = 0;

    for (u32 i = 0; i < n && (!real || !fake); i++) {
        if (tlb_is_real[i] && !real)
            real = pages[i];
        else if (!tlb_is_real[i] && !fake)
            fake = pages[i];
    }
    if (!real || !fake) {
        printf(INFO "Prefetcher method: needs one real and one fake huge page, found only %s; using the TLB method\n",
               real ? "real" : "fake");
        return;
    }

    start = time_us();
    if (!pref_find_config(real, fake, &cfg)) {
        printf(INFO "Prefetcher method: no reliable configuration on this processor (search took %.3fms); using the TLB method\n",
               (time_us() - start) / 1e3);
        return;
    }
    printf(INFO "Prefetcher method: %u accesses %uB apart, probe line %uB beyond; prefetched on %s huge pages; difference %u%% (search took %.3fms)\n",
           cfg.n_train, cfg.stride, cfg.n_jump * cfg.stride,
           cfg.real_hits ? "real" : "fake", cfg.diff_pct,
           (time_us() - start) / 1e3);

    start = time_us();
    for (u32 i = 0; i < n; i++)
        agree += pref_is_real(&cfg, pages[i], rounds) == tlb_is_real[i];
    printf(INFO "Prefetcher probing stage took: %.3fms; agrees with the TLB method on %u/%u pages\n",
           (time_us() - start) / 1e3, agree, n);
}

static i32 run_tlb_mode(u32 pages_to_test, u32 samples_per_page, HpKind hp_kind, u32 core_limit, bool try_pref, const char* append_results_path)
{
    u32 avail_cores = (u32)n_system_cores();
    u32 max_cores = core_limit == 0 ? avail_cores : min(core_limit, avail_cores);
    u32 tlb_threshold = TLB_THRESHOLD_DEFAULT;
    u8** hp_pages = NULL;
    u8** eligible_pages = NULL;
    bool* hp_eligible = NULL;
    u64* page_medians = NULL;
    bool* page_is_real = NULL;
    pthread_t* workers = NULL;
    TlbProbeWorkerCtx* worker_ctx = NULL;
    u32 workers_started = 0;
    u32 eligible_count = 0;
    u32 worker_count = 0;
    u64 tested = 0;
    u64 real_pages = 0;
    u64 fake_pages = 0;
    u64 alloc_failed = 0;
    u64 thp_alloc_ok = 0;
    u64 thp_promoted = 0;
    u64 thp_omitted_madv_fail = 0;
    u64 thp_omitted_not_huge = 0;
    u64 gt_match = 0;
    u64 gt_mismatch = 0;
    bool printed_real_v1 = false;
    bool printed_fake_v1 = false;
    i32 ret = EXIT_FAILURE;
    u64 run_start_us = time_us();

    if (ptedit_init()) {
        fprintf(stderr, ERR "ptedit_init failed (is the PTEditor kernel module loaded?)\n");
        return EXIT_FAILURE;
    }

    u32 tlb_miss=0, tlb_hit=0;
    u64 s = time_us();
    tlb_threshold = tlb_probe_threshold_cycles(hp_kind, &tlb_miss, &tlb_hit);
    if (verbose)
        printf(V1 "TLB threshold calculation took: %fms\n", (time_us() - s) / 1e3);

    hp_pages = calloc(pages_to_test, sizeof(*hp_pages));
    hp_eligible = calloc(pages_to_test, sizeof(*hp_eligible));
    if (!hp_pages || !hp_eligible) {
        fprintf(stderr, ERR "Failed to allocate huge page tracking buffers\n");
        goto out;
    }

    if (verbose)
        printf(V1 INFO "TLB mode: hp=%s pages=%u samples=%u\n",
            hp_kind == HP_KIND_HUGETLB ? "hugetlb" : "thp",
            pages_to_test,
            samples_per_page);
    if (verbose)
        printf(V1 INFO "TLB workers: requested=%u available=%u using_up_to=%u\n",
               core_limit,
               avail_cores,
               max_cores);
    printf(INFO "Calculated TLB threshold: %u cycles (miss: %u, hit: %u)\n",
           tlb_threshold, tlb_miss, tlb_hit);

    for (u32 i = 0; i < pages_to_test; i++) {
        u8* hp = NULL;

        if (hp_kind == HP_KIND_HUGETLB) {
            hp = mmap_huge_private_init(NULL, HUGE_PAGE_SIZE, 'a');
        } else {
            hp = mmap_anon_thp_init(NULL, HUGE_PAGE_SIZE, 0x5A);
            if (hp)
                thp_alloc_ok++;
        }

        if (!hp) {
            alloc_failed++;
            if (hp_kind == HP_KIND_HUGETLB) {
                fprintf(stderr, ERR "Probe #%u hugetlb allocation failed; aborting remaining probes\n", i);
                goto out;
            }
            thp_omitted_madv_fail++;
            continue;
        }

        hp_pages[i] = hp;

        if (hp_kind == HP_KIND_THP) {
            bool promoted = is_thp_promoted(hp);
            if (!promoted) {
                thp_omitted_not_huge++;
                continue;
            }
            thp_promoted++;
        }

        hp_eligible[i] = true;
        eligible_count++;
    }

    if (eligible_count == 0)
        goto summarize;

    worker_count = min(eligible_count, max_cores);
    eligible_pages = calloc(eligible_count, sizeof(*eligible_pages));
    page_medians = calloc(eligible_count, sizeof(*page_medians));
    page_is_real = calloc(eligible_count, sizeof(*page_is_real));
    workers = calloc(worker_count, sizeof(*workers));
    worker_ctx = calloc(worker_count, sizeof(*worker_ctx));
    if (!eligible_pages || !page_medians || !page_is_real || !workers || !worker_ctx) {
        fprintf(stderr, ERR "Failed to allocate probe worker buffers\n");
        goto out;
    }

    for (u32 i = 0, w = 0; i < pages_to_test; i++) {
        if (hp_eligible[i])
            eligible_pages[w++] = hp_pages[i];
    }

    {
        u32 base = eligible_count / worker_count;
        u32 rem = eligible_count % worker_count;
        u32 start = 0;

        for (u32 t = 0; t < worker_count; t++) {
            u32 cnt = base + (t < rem ? 1 : 0);
            worker_ctx[t].pages = eligible_pages;
            worker_ctx[t].medians = page_medians;
            worker_ctx[t].is_real = page_is_real;
            worker_ctx[t].start_idx = start;
            worker_ctx[t].end_idx = start + cnt;
            worker_ctx[t].samples_per_page = samples_per_page;
            worker_ctx[t].tlb_threshold = tlb_threshold;
            worker_ctx[t].core_id = (i32)t;
            worker_ctx[t].err = 0;
            worker_ctx[t].probe_us = 0;
            start += cnt;

            if (pthread_create(&workers[t], NULL, tlb_probe_worker, &worker_ctx[t])) {
                fprintf(stderr, ERR "Failed to create TLB probe worker #%u\n", t);
                goto out;
            }
            workers_started++;
        }
    }

    for (u32 t = 0; t < workers_started; t++) {
        if (pthread_join(workers[t], NULL) != 0) {
            fprintf(stderr, ERR "Failed to join TLB probe worker #%u\n", t);
            goto out;
        }
    }

    for (u32 t = 0; t < workers_started; t++) {
        if (worker_ctx[t].err) {
            fprintf(stderr, ERR "TLB probe worker #%u failed\n", t);
            goto out;
        }
    }
    if (verbose) {
        printf(V1 INFO "TLB page distribution: ");
        for (u32 t = 0; t < worker_count; t++) {
            u32 n_pages_t = worker_ctx[t].end_idx - worker_ctx[t].start_idx;
            printf("t%u=%u", t, n_pages_t);
            if (t + 1 < worker_count)
                printf(" | ");
        }
        printf("\n");
    }
    workers_started = 0;
    printf(INFO "TLB probing stage took: ");
    u64 total_probe_us = 0;
    for (u32 t = 0; t < worker_count; t++) {
        printf("t%u=%.3fms", t, worker_ctx[t].probe_us / 1e3);
        total_probe_us += worker_ctx[t].probe_us;
        if (t + 1 < worker_count)
            printf(" | ");
    }
    printf(" | total=%.3fms\n", total_probe_us / 1e3);

    if (try_pref)
        run_pref_stage(eligible_pages, page_is_real, eligible_count, samples_per_page);

    for (u32 i = 0; i < eligible_count; i++) {
        u8* hp;
        u64 median;
        bool is_real;

        hp = eligible_pages[i];
        median = page_medians[i];
        is_real = page_is_real[i];
        HpaInfo hpa_info;

        if (debug) {
            if (va_to_hpa_info(hp, &hpa_info) != 0) {
                fprintf(stderr, ERR "Failed to query HPA info for debug output\n");
                goto out;
            }
            if (hpa_flags_is_real(hpa_info.flags) == is_real)
                gt_match++;
            else
                gt_mismatch++;
        }

        if (verbose >= 2 || (verbose == 1 && ((is_real && !printed_real_v1) || (!is_real && !printed_fake_v1)))) {
            printf(V1 "#%llu Userspace VA=%p median=%llu => %s%s%s",
                   (ull)tested,
                   hp,
                   (ull)median,
                   is_real ? GRN : RED,
                   is_real ? "REAL" : "FAKE",
                   RST);
            printf("\n");

            if (debug)
                printf(D1 "HPA=0x%llx GT=%s order=%u flags=0x%llx\n",
                       (ull)hpa_info.hpa,
                       hpa_flags_is_real(hpa_info.flags) ? "REAL" : "FAKE",
                       hpa_flags_order(hpa_info.flags),
                       (ull)hpa_info.flags);
        }

        if (verbose == 1) {
            if (is_real)
                printed_real_v1 = true;
            else
                printed_fake_v1 = true;
        }

        if (is_real)
            real_pages++;
        else
            fake_pages++;

        tested++;
    }

summarize:
    if (hp_kind == HP_KIND_THP && verbose > 2) {
        printf(V3 "THP summary: requested=%u madvise_alloc_ok=%llu vm_huge=%llu omitted_due_to_madv=%llu\n",
               pages_to_test,
               (ull)thp_alloc_ok,
               (ull)thp_promoted,
               (ull)(thp_omitted_madv_fail + thp_omitted_not_huge));
        printf(V3 "THP omitted breakdown: madvise_alloc_fail=%llu not_vm_huge_after_madvise=%llu\n",
               (ull)thp_omitted_madv_fail,
               (ull)thp_omitted_not_huge);
    }

    if (hp_kind == HP_KIND_HUGETLB) {
        printf(INFO "Userspace TLB summary: probed=%llu real=%llu fake=%llu\n",
               (ull)tested,
               (ull)real_pages,
               (ull)fake_pages);
    } else {
        printf(INFO "Userspace TLB summary: probed=%llu real=%llu fake=%llu alloc_failed=%llu\n",
               (ull)tested,
               (ull)real_pages,
               (ull)fake_pages,
               (ull)alloc_failed);
    }
    if (verbose >= 2) {
        printf(V2 "TLB calibration: threshold=%u cycles miss=%u cycles hit=%u cycles\n",
               tlb_threshold,
               tlb_miss,
               tlb_hit);
    }
    if (debug) {
        f64 gt_acc = tested ? ((f64)gt_match * 100.0) / (f64)tested : 0.0;

        printf(INFO "GT summary: checked=%llu match=%s%llu%s mismatch=%s%llu%s accuracy=%.2f%%\n",
               (ull)(gt_match + gt_mismatch),
               GRN,
               (ull)gt_match,
               RST,
               gt_mismatch == 0 ? GRN : RED,
               (ull)gt_mismatch,
               RST,
               gt_acc);
    }

    if (!append_tlb_results(append_results_path, hp_kind, pages_to_test, tested,
                            real_pages, fake_pages, alloc_failed, tlb_threshold,
                            tlb_miss, tlb_hit, samples_per_page, core_limit,
                            time_us() - run_start_us))
        goto out;

    ret = EXIT_SUCCESS;

out:
    for (u32 t = 0; t < workers_started; t++)
        pthread_join(workers[t], NULL);

    if (hp_pages) {
        for (u32 i = 0; i < pages_to_test; i++) {
            if (hp_pages[i])
                munmap(hp_pages[i], HUGE_PAGE_SIZE);
        }
    }
    free(worker_ctx);
    free(workers);
    free(page_is_real);
    free(page_medians);
    free(eligible_pages);
    free(hp_pages);
    free(hp_eligible);
    ptedit_cleanup();
    return ret;
}

static void print_usage(const char* progname)
{
    printf("Usage: %s <tlb|pref> [options]\n"
           "\n"
           "Modes:\n"
           "  tlb                     Classify huge pages with the TLB method\n"
           "  pref                    Classify with the TLB method, then search for a prefetcher\n"
           "                          configuration and compare; keeps the TLB results if none is reliable\n"
           "\n"
           "Options:\n"
           "  -n, --num-pages N       Number of huge pages to test [default: 1]\n"
           "  -c, --num-core N        Max number of cores (workers) used for TLB probing [default: all]\n"
           "  -S, --samples N         Samples per page before median [default: %u]\n"
           "  --hp TYPE               Huge page allocation type: thp|hugetlb [default: thp]\n"
           "  --append-results PATH   Append one tab-separated summary row per completed run\n"
           "  -v, --verbose LEVEL     Show statistics and logs (1=basic, 2=detailed, 3=very detailed) [default: 0]\n"
           "  -d, --debug             Compare with the host's backing; needs misc/host/gpa_hpa_km on the host and guest\n"
           "  -h, --help              Display this help message and exit\n",
           progname, TLB_SAMPLES_DEFAULT);
}

i32 main(i32 argc, char* argv[])
{
    u64 start_full = time_us();
    i32 opt;
    HpKind hp_kind = HP_KIND_THP;
    u32 n_pages = 1;
    u32 samples = TLB_SAMPLES_DEFAULT;
    u32 core_limit = 0;
    const char* append_results_path = NULL;
    bool try_pref;
    i32 result;

    struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"verbose", required_argument, 0, 'v'},
        {"debug", no_argument, 0, 'd'},
        {"num-pages", required_argument, 0, 'n'},
        {"num-core", required_argument, 0, 'c'},
        {"samples", required_argument, 0, 'S'},
        {"hp", required_argument, 0, OPT_HP_KIND},
        {"append-results", required_argument, 0, OPT_APPEND_RESULTS},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "hv:dn:c:S:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'h':
            print_usage(argv[0]);
            return EXIT_SUCCESS;
        case 'v': {
            u32 val;
            if (!parse_u32_opt("-v/--verbose", optarg, &val))
                return EXIT_FAILURE;
            if (val > 3) {
                fprintf(stderr, ERR "--verbose must be 0-3\n");
                return EXIT_FAILURE;
            }
            verbose = (i32)val;
            break;
        }
        case 'd':
            debug = true;
            break;
        case 'n':
            if (!parse_u32_opt("-n/--num-pages", optarg, &n_pages))
                return EXIT_FAILURE;
            if (n_pages == 0) {
                fprintf(stderr, ERR "--num-pages must be greater than 0\n");
                return EXIT_FAILURE;
            }
            break;
        case 'c':
            if (!parse_u32_opt("-c/--num-core", optarg, &core_limit))
                return EXIT_FAILURE;
            if (core_limit == 0) {
                fprintf(stderr, ERR "--num-core must be greater than 0\n");
                return EXIT_FAILURE;
            }
            break;
        case 'S':
            if (!parse_u32_opt("-S/--samples", optarg, &samples))
                return EXIT_FAILURE;
            if (samples == 0) {
                fprintf(stderr, ERR "--samples must be greater than 0\n");
                return EXIT_FAILURE;
            }
            break;
        case OPT_HP_KIND:
            if (!parse_hp_kind(optarg, &hp_kind)) {
                fprintf(stderr, ERR "Invalid --hp value: %s (expected: thp|hugetlb)\n", optarg);
                return EXIT_FAILURE;
            }
            break;
        case OPT_APPEND_RESULTS:
            if (!*optarg) {
                fprintf(stderr, ERR "--append-results requires a non-empty path\n");
                return EXIT_FAILURE;
            }
            append_results_path = optarg;
            break;
        default:
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, ERR "Missing required mode argument (tlb|pref)\n");
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (strcmp(argv[optind], "tlb") == 0) {
        try_pref = false;
    } else if (strcmp(argv[optind], "pref") == 0) {
        try_pref = true;
    } else {
        fprintf(stderr, ERR "Invalid mode: %s (expected: tlb|pref)\n", argv[optind]);
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (debug && start_debug_mod() != 0) {
        fprintf(stderr, ERR "Failed to open/start debugging module. Exiting.\n");
        return EXIT_FAILURE;
    }

    result = run_tlb_mode(n_pages, samples, hp_kind, core_limit, try_pref, append_results_path);

    if (debug)
        stop_debug_mod();

    printf(INFO "Program completed | %.3fs\n", (time_us() - start_full) / 1e6);

    return result;
}

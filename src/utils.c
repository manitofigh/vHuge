#include "../include/utils.h"
#include <sys/sysinfo.h>
#include <sys/time.h>

i32 verbose;

static i32 compare_u64(const void* a, const void* b)
{
    return (*(u64*)a > *(u64*)b) - (*(u64*)a < *(u64*)b);
}

// sorts nums_arr in place
u64 calc_median_u64(u64* nums_arr, u32 cnt)
{
    if (cnt == 0)
        return 0;

    qsort(nums_arr, cnt, sizeof(*nums_arr), compare_u64);

    if (cnt % 2)
        return nums_arr[cnt / 2];
    return (nums_arr[cnt / 2] + nums_arr[(cnt / 2) - 1]) / 2;
}

i32 set_cpu_affinity(u16 core_id)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    return (sched_setaffinity(0, sizeof(mask), &mask));
}

// in microseconds
u64 time_us(void) 
{ 
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (u64)tv.tv_sec * 1000000LL + (u64)tv.tv_usec;
}

i32 n_system_cores(void)
{
    i32 n_cores = get_nprocs();
    if (n_cores <= 0) {
        fprintf(stderr, ERR "failed to detect the number of cores. assuming 2 cores.\n");
        return 2;
    }
    return n_cores;
}

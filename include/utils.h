#ifndef UTILS_H
#define UTILS_H

#include "common.h"
#include "mem.h"
#include "asm.h"

extern i32 verbose;

u64 calc_median_u64(u64* nums_arr, u32 cnt);

i32 set_cpu_affinity(u16 core_id);

u64 time_us(void);

i32 n_system_cores(void);

#endif // UTILS_H

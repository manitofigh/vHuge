#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE // cpu affinity
#include "colors.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int32_t i32;

typedef unsigned long long ull;
typedef double f64;

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define PAGE_SIZE          (1<<12) // 4KiB
#define HUGE_PAGE_SIZE            (1<<21) // 2MiB
#define CL_SIZE            (1UL<<6)  // 64 bytes

#endif // COMMON_H

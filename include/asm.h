#ifndef ASM_H
#define ASM_H

#include "common.h"

#define ALWAYS_INLINE inline __attribute__((always_inline))

#define maccess(P)                            \
    do {                                      \
        typeof(*(P)) _NO_USE;                 \
        __asm__ __volatile__("mov (%1), %0\n" \
                             : "=r"(_NO_USE)  \
                             : "r"(P)         \
                             : "memory");     \
    } while (0)

static ALWAYS_INLINE void _clflushopt(void *addr)
{
    __asm__ __volatile__("clflushopt (%0)"
                         : 
                         : "r"(addr) 
                         : "memory");
}

static ALWAYS_INLINE void _mfence()
{
    __asm__ __volatile__("mfence"
                         :
                         :
                         : "memory");
}

static ALWAYS_INLINE void _lfence(void)
{
    __asm__ __volatile__("lfence"
                         :
                         :
                         : "memory");
}

static ALWAYS_INLINE u64 _rdtsc(void)
{
    u64 rax;
    __asm__ __volatile__(
        "rdtsc\n\t"
        "shl $32, %%rdx\n\t"
        "or %%rdx, %0\n\t"
        :"=a"(rax)
        :: "rdx", "memory", "cc"
    );
    return rax;
}

// https://github.com/google/highwayhash/blob/master/highwayhash/tsc_timer.h
static ALWAYS_INLINE u64 timer_start(void)
{
    u64 t;
    __asm__ __volatile__("mfence\n\t"
                         "lfence\n\t"
                         "rdtsc\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         : "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 timer_stop(void)
{
    u64 t;
    __asm__ __volatile__("rdtscp\n\t"
                         "shl $32, %%rdx\n\t"
                         "or %%rdx, %0\n\t"
                         "lfence"
                         : "=a"(t)
                         :
                         : "rcx", "rdx", "memory", "cc");
    return t;
}

static ALWAYS_INLINE u64 _timer_warmup(void)
{
    return _rdtsc();
}

static ALWAYS_INLINE u64 time_maccess(u8* p)
{
    _timer_warmup();
    u64 st = timer_start();
    maccess(p);
    u64 end = timer_stop();
    return end - st;
}

#endif // ASM_H

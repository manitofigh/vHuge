#ifndef GPA_HPA_H
#define GPA_HPA_H

#include "common.h"

#define DEBUG_MOD_PATH "/proc/gpa_hpa"

typedef struct {
    u64 hpa;
    u64 flags;
} HpaInfo;

// flag layout written by the host patch in misc/host/gpa_hpa_km/hc.diff
#define HPA_FLAG_HUGE      (1ULL << 0) // part of a huge or compound page
#define HPA_FLAG_MAPPED    (1ULL << 1)
#define HPA_FLAG_ORDER_SH  2           // folio order of a huge page
#define HPA_FLAG_ORDER_MSK 0x1FULL
#define HPA_HUGE_ORDER     9           // 2 MiB

i32 start_debug_mod(void);

void stop_debug_mod(void);

i32 va_to_hpa_info(void* va, HpaInfo* out);

static inline u32 hpa_flags_order(u64 flags)
{
    return (u32)((flags >> HPA_FLAG_ORDER_SH) & HPA_FLAG_ORDER_MSK);
}

// backed by a host huge page of at least 2 MiB
static inline bool hpa_flags_is_real(u64 flags)
{
    return (flags & HPA_FLAG_HUGE) && hpa_flags_order(flags) >= HPA_HUGE_ORDER;
}

#endif /* GPA_HPA_H */

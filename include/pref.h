#ifndef PREF_H
#define PREF_H

#include "common.h"

/*
 * Prefetcher method: on some processors the prefetcher prefetches a line
 * beyond a short access pattern on a real huge page but not on a fake one
 * (or the reverse). A configuration is one such pattern.
 */
typedef struct {
    u32 n_train;      // accesses that train the prefetcher
    u32 stride;       // bytes between two accesses
    u32 n_jump;       // probe line is n_jump strides beyond the last access
    bool real_hits;   // true: the probe line is prefetched on a real huge page
    u32 diff_pct;     // hit-rate difference between the two reference pages
} PrefConfig;

/*
 * Search for a configuration that tells `real` from `fake`, two huge pages
 * already classified by the TLB method. Returns false when no configuration
 * is reliable; the caller then keeps using the TLB method.
 */
bool pref_find_config(u8 *real, u8 *fake, PrefConfig *out);

/* Classify one huge page with a configuration found above. */
bool pref_is_real(const PrefConfig *cfg, u8 *hp, u32 rounds);

#endif // PREF_H

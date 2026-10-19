#include "../include/pref.h"
#include "../include/asm.h"

// search space: 8 x 4 x 4 = 128 configurations
#define TRAIN_MIN        5
#define TRAIN_MAX        12
#define STRIDE_MIN       64
#define STRIDE_MAX       256
#define JUMP_MIN         1
#define JUMP_MAX         4

#define SCREEN_ROUNDS    20
#define SCREEN_MIN_DIFF  18    // rounds in which the two pages must differ
#define FINAL_ROUNDS     1000
#define MIN_DIFF_PCT     90    // below this the configuration is not reliable
#define HIT_CYCLES       100   // probe line was prefetched if faster than this
#define RESET_LINES      64

/* One round: flush the lines, run the access pattern, time the probe line. */
static bool pref_round(u8 *p, u32 n_train, u32 stride, u32 n_jump)
{
    u32 probe = stride * (n_train + n_jump);
    u32 last = max(probe, (u32)(RESET_LINES - 1) * CL_SIZE);

    for (u32 off = 0; off <= last; off += CL_SIZE)
        _clflushopt(p + off);
    _mfence();
    for (u32 i = 0; i < n_train; i++)
        maccess(p + i * stride);
    _mfence();
    return time_maccess(p + probe) < HIT_CYCLES;
}

static u32 pref_hits(u8 *p, const PrefConfig *c, u32 rounds)
{
    u32 hits = 0;

    for (u32 r = 0; r < rounds; r++)
        hits += pref_round(p, c->n_train, c->stride, c->n_jump);
    return hits;
}

bool pref_find_config(u8 *real, u8 *fake, PrefConfig *out)
{
    PrefConfig best = {0};

    for (u32 n_train = TRAIN_MIN; n_train <= TRAIN_MAX; n_train++) {
        for (u32 stride = STRIDE_MIN; stride <= STRIDE_MAX; stride += CL_SIZE) {
            for (u32 n_jump = JUMP_MIN; n_jump <= JUMP_MAX; n_jump++) {
                PrefConfig c = { .n_train = n_train, .stride = stride,
                                 .n_jump = n_jump };
                u32 r = pref_hits(real, &c, SCREEN_ROUNDS);
                u32 f = pref_hits(fake, &c, SCREEN_ROUNDS);

                if ((r > f ? r - f : f - r) < SCREEN_MIN_DIFF)
                    continue;

                r = pref_hits(real, &c, FINAL_ROUNDS);
                f = pref_hits(fake, &c, FINAL_ROUNDS);
                c.real_hits = r > f;
                c.diff_pct = (r > f ? r - f : f - r) * 100 / FINAL_ROUNDS;
                if (c.diff_pct > best.diff_pct)
                    best = c;
            }
        }
    }

    if (best.diff_pct < MIN_DIFF_PCT)
        return false;
    *out = best;
    return true;
}

bool pref_is_real(const PrefConfig *cfg, u8 *hp, u32 rounds)
{
    bool hit = 2 * pref_hits(hp, cfg, rounds) > rounds;

    return hit == cfg->real_hits;
}

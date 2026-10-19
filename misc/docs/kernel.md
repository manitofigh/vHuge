# vHuge Kernel (6.15.2-vhuge)

A huge page in a VM is _real_ when the host also backs it with a huge page, and
_fake_ when the host backs it with base pages. Only real huge pages remove the
extra TLB misses that huge pages are meant to avoid, so the vHuge kernel tries
to give them to applications first.

To do this, the kernel keeps a pool of huge pages that are known to be real.
The `vhuge-poold` daemon fills the pool: it allocates huge pages, checks each
one with the TLB or prefetcher method, and hands the real ones to the kernel.
When an application then needs an anonymous huge page, the kernel takes one
from the pool if it can, and otherwise allocates a huge page the normal way.

When the host has few real huge pages left, the pool runs dry. The kernel then
uses DAMON, Linux's memory access monitor, to find which huge pages
applications use most (_hot_ pages). From that point on, real huge pages go
only to hot data. The kernel also checks hot huge pages that did not come from
the pool, and moves the hot data on fake huge pages onto real ones. This part
is built when `CONFIG_VHUGE_DAMON=y`.

The kernel release string is `6.15.2-vhuge`. Everything below can be read and
changed at run time through files under `/sys/kernel/vhuge_pool/`, for example:

```sh
cat /sys/kernel/vhuge_pool/current
echo 1024 | sudo tee /sys/kernel/vhuge_pool/target
```

## Pool

- `enabled`: whether vHuge is on. `1` (default) lets the pool collect real
  huge pages and hand them out. `0` returns every pooled page to the normal
  page allocator, and all huge page allocations go back to the normal kernel
  behavior.
- `current` (read-only): the number of real huge pages in the pool right now.
- `target`: the number of pages the daemon fills the pool up to. Default: `512`.
- `low_watermark`: the daemon starts filling the pool when `current` drops
  below this number, and then fills it up to `target`. The default is 2% of the
  free memory at boot, counted in huge pages, and at most `512`.
- `max`: the most pages the pool may ever hold, whatever the other settings
  say. Default: `8192`.
- `fill_to`: a one-time request to fill the pool to this many pages, for
  example before a measurement. It must be lower than `max`, and reads `0` when
  no request is pending. It works in both policy modes (below) and does not
  change the DAMON state.

## Choosing which data gets real huge pages

vHuge has two policy modes, set with `damon_policy_mode`:

- **Automatic (`0`, default).** While the daemon keeps the pool filled, every
  anonymous huge page request can take a real page. The first time a refill
  fails, the kernel starts DAMON (_scout_ mode) and keeps the remaining pool as
  a reserve that cold or unknown data cannot use. After
  `damon_pressure_failures` failed refills in a row, it enters _pressure_ mode,
  in which only data that DAMON found hot gets real pages. Once the pool
  recovers, DAMON stops and vHuge goes back to handing out real pages freely.
- **Fixed budget (`1`).** Gives one workload a set number of real huge pages
  (`damon_hot_budget_pages`) and spends them on its hottest data, regardless
  of how full the pool is. This is meant for controlled experiments.
  `scripts/start-fixed-budget.sh N` sets this up. Writing `1` again starts a
  new run once the previous workload's real pages are gone; until then the
  write fails with `EBUSY`.

Settings for both modes:

- `damon_enabled`: whether DAMON may be started at all. Default: `1`.
- `damon_policy_reset`: write `1` to start over: in automatic mode this clears
  the count of failed refills, and in fixed-budget mode it re-arms the run.

Automatic mode:

- `damon_pressure_failures`: failed refills in a row before scout mode turns
  into pressure mode. Default: `3`.
- `damon_migration_reserve`: the smallest reserve kept for hot data when scout
  mode starts. Default: `32` pages.
- `damon_stop_watermark_pct`: how far the pool must recover before pressure
  mode ends, as a percentage of the way from `low_watermark` to `target`.
  Default: `50`.
- `damon_min_runtime_sec`: the shortest time pressure mode runs before it may
  end. Default: `30`.

Fixed-budget mode:

- `damon_hot_budget_pages`: the number of real huge pages the workload may
  receive. Default: `0`.
- `damon_fixed_scout_aggregations`: how many DAMON measurement rounds to wait
  before spending the budget, so that the hot data is known first. Default:
  `3`.
- `damon_budget_fcfs`: `1` spends the budget on the workload's huge page
  requests in the order they arrive, without DAMON. This is the "vHuge without
  DAMON" comparison and applies to tasks run under `numactl --membind`.
  Default: `0`.

## Finding hot data with DAMON

DAMON samples the memory of the processes that use huge pages and, after each
measurement round (an _aggregation_), reports how often each region was
accessed.

- `damon_sample_us`: time between two access samples. Default: `20000` µs.
- `damon_aggr_us`: length of one measurement round. Default: `500000` µs.
- `damon_update_us`: how often DAMON updates the list of memory regions it
  watches. Default: `1000000` µs.
- `damon_min_regions`, `damon_max_regions`: the fewest and most regions DAMON
  splits the watched memory into. Defaults: `128` and `2048`.
- `damon_hot_threshold_bp`: how often a region must be accessed to count as
  hot, in hundredths of a percent of the samples. Default: `5000` (50%).
- `damon_hot_pmd_max`: the most hot huge pages tracked at once. Default and
  upper limit: `8192`.

## Checking hot huge pages in the kernel

Hot huge pages that did not come from the pool may still be real. Once per
second, the kernel picks such pages and checks them with the TLB method, using
the same test as the daemon. Pages found real are counted as real from then on.

- `damon_probe_samples`: timed accesses per checked page. Default: `5`. The
  daemon sets it to its own `--samples` value when it starts.
- `damon_probe_self_calibrate`: `1` (default) makes the kernel measure the
  latency of a TLB miss and a TLB hit itself, set the threshold from them, and
  compare each access with a TLB hit measured right after it, so that changes
  in the machine's overall latency cancel out. `0` compares the access latency
  with `damon_probe_threshold_cycles` directly. Reading the file shows the
  setting and the last measurements, for example `1 hit=32 miss=70`.
- `damon_probe_threshold_cycles`: the latency, in CPU cycles, above which an
  access counts as a TLB miss and the page as fake. Default: `55`. The daemon
  writes its own threshold when it starts. With self-calibration on, a write
  makes the kernel calibrate again and store `3 * (miss - hit) / 5` here.
- `damon_probe_max_per_scan`: the most pages checked per second. Default: `16`.
  `0` pauses the checks but keeps DAMON running.
- `damon_probe_cooldown_sec`: how long to wait before checking a page found
  fake again. Default: `60`.

## Moving hot data onto real huge pages

When hot data sits on a fake huge page and the pool has a real one, the kernel
copies the data to the real page with the normal page migration code.

- `damon_migrate_max_per_scan`: the most pages moved per second. Default: `16`.
  `0` pauses migration but keeps DAMON and the checks running.

## Status

`/proc/vhuge` shows the state of the pool, the policy, DAMON, the in-kernel
checks and migration, and a table of how many real huge pages each process
owns.

- `proc_verbosity`: `0` (default) shows the current state only. `1` also shows
  totals since the last reset and timing details.
- `reset_stats`: write `1` to set the totals in `/proc/vhuge` back to zero.

The daemon talks to the kernel through `/dev/vhuge_pool`, which only root can
open; it is not meant to be used directly.

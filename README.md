# vHuge

vHuge probes whether an allocated huge page in a VM is also backed by huge
pages on the host (referred to as a _real_ huge page), or is backed by host
base pages (a _fake_ huge page).

If you want to contribute to this project, please read [CONTRIBUTING.md](misc/docs/CONTRIBUTING.md) first!

## Setup

### Build 

In an x86-64 VM, and from the repository's root, run:
```bash
./scripts/setup.sh all  # install dependencies, build vhuge, and load dependencies
```

### Run the userspace prober
```bash
cd build                 
sudo ./vhuge tlb -n 256  # classify 256 transparent huge pages
```

The summary near the end of the output gives results like:

```
[INFO] Calculated TLB threshold: 50 cycles (miss: 62, hit: 32)
...
[INFO] Userspace TLB summary: probed=256 real=71 fake=185 alloc_failed=0
[INFO] Program completed | 0.268s
```

There are two ways to probe real/fake hugepages: `tlb` and `pref`.
1. `tlb` (TLB method) uses the TLB hit/miss threshold latency to tell whether an 
accessed hugepage is hugepage-backed or not. This method works both on VMs running on 
Intel and AMD processors.
2. `pref` (Prefetcher method) exploits the subtle difference cache line prefetchers show 
when data is accessed on a hugepage vs base page. Please note that the prefetcher method 
is only tested and validated on newer Intel Xeon scalable processors (Gen 5 Emerald Rapids). 
This is because older generations do not show any difference in prefetching pattern 
for base vs. hugepages, and thus make this method unusable on them.

Use `vhuge -h` to see list of all options available.

## vHuge hugepage management system

vHuge can further enable an anonymous hugepage management system where
anonymous hugepage requests are only given real huge pages as long as
available. If real huge pages are scarce, vHuge uses real huge pages to only
back _hot_ application data rather than freely hand them out for every
request.

If you would like to set up vHuge's end to end hugepage management system,
follow the following commands inside of your VM:

```bash
./scripts/setup.sh all      # if not done already in the previous section
./scripts/setup.sh kernel   # fetch Linux 6.15.2, add the vHuge changes, build and install it
sudo reboot                 # boot 6.15.2-vhuge (check with uname -r)
./scripts/setup.sh daemon   # build PTEditor and vhuge-poold, install and start the service
cat /proc/vhuge             # you should be able to see the pool size, probes, and other options
```

The values exposed under `/proc/vhuge` contain information on the filtered real/fake 
huge pages in the VM. You can see the daemon's full logs at `/var/log/vhuge-poold.log`.

| Script | What it does |
|---|---|
| `scripts/start-vhuge.sh`, `scripts/shutdown-vhuge.sh` | turn vHuge on and off |
| `scripts/start-damon.sh`, `scripts/shutdown-damon.sh` | turn hot huge page repair on and off |
| `scripts/vhuge-poold-{start,shutdown,restart}.sh` | control the daemon and reset the pool |
| `scripts/start-fixed-budget.sh N` | give repair a fixed budget of N real huge pages |

[`misc/docs/kernel.md`](misc/docs/kernel.md) explains how the kernel side works and describes every setting
under `/sys/kernel/vhuge_pool`. You can similarly use `vhuge-poold --help` to see the 
daemon's options.

By default, since the prefetcher method is ~10x faster in classification vs. the TLB method, 
`poold` attempts to see if it can observe a prefetching behavior difference. If you want to 
use the TLB method nonetheless and skip this attempt (if you already know your processor 
is not supported), you can pass `--tlb-only`.

## License

vHuge is released under the [PolyForm Noncommercial License 1.0.0](LICENSE),
which allows noncommercial use and forbids commercial use. The kernel changes
(`linux-6.15.2/`) and the host module (`misc/host/`) are under GPL-2.0, as
they are part of or built against Linux.

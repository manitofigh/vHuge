#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

POOL_DIR="${VHUGE_SYSFS:-/sys/kernel/vhuge_pool}"

[[ -d "${POOL_DIR}" ]] || {
    echo "missing ${POOL_DIR}; boot the vHuge kernel first" >&2
    exit 1
}

write_knob()
{
    local knob=$1
    local value=$2
    local path="${POOL_DIR}/${knob}"

    [[ -w "${path}" ]] || {
        echo "missing writable ${path}" >&2
        exit 1
    }
    printf '%s\n' "${value}" > "${path}"
}

# Reset the DAMON policy settings to the kernel defaults, then enable it.
write_knob damon_enabled 0
write_knob damon_pressure_failures "${VHUGE_DAMON_PRESSURE_FAILURES:-3}"
write_knob damon_stop_watermark_pct "${VHUGE_DAMON_STOP_WATERMARK_PCT:-50}"
write_knob damon_min_runtime_sec "${VHUGE_DAMON_MIN_RUNTIME_SEC:-30}"
write_knob damon_migration_reserve "${VHUGE_DAMON_MIGRATION_RESERVE:-32}"
write_knob damon_hot_threshold_bp "${VHUGE_DAMON_HOT_THRESHOLD_BP:-5000}"
write_knob damon_probe_max_per_scan \
    "${VHUGE_DAMON_PROBE_MAX_PER_SCAN:-16}"
write_knob damon_migrate_max_per_scan \
    "${VHUGE_DAMON_MIGRATE_MAX_PER_SCAN:-16}"
write_knob damon_hot_budget_pages "${VHUGE_DAMON_HOT_BUDGET_PAGES:-0}"
write_knob damon_fixed_scout_aggregations \
    "${VHUGE_DAMON_FIXED_SCOUT_AGGREGATIONS:-3}"
if ! write_knob damon_policy_mode "${VHUGE_DAMON_POLICY_MODE:-0}" 2>/dev/null; then
    # A fixed-budget run starts only when no process owns real huge pages.
    echo "cannot set damon_policy_mode=${VHUGE_DAMON_POLICY_MODE:-0}: processes" \
        "still own real huge pages; stop them and run this again" >&2
    awk '/^PID/ {p = 1; next} p && $1 != "-"' /proc/vhuge >&2
    write_knob damon_enabled 1
    exit 1
fi
write_knob damon_enabled 1

if [[ -r /proc/vhuge ]]; then
    grep -E '^(damon|damon_policy|fixed_budget):' /proc/vhuge || true
fi

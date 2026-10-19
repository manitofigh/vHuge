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

write_if_present()
{
    local knob=$1
    local value=$2
    local path="${POOL_DIR}/${knob}"

    if [[ -w "${path}" ]]; then
        printf '%s\n' "${value}" > "${path}"
    fi
}

write_if_present damon_enabled 0
write_if_present damon_policy_mode 0
write_if_present damon_policy_reset 1

if [[ -r /proc/vhuge ]]; then
    grep -E '^(damon|damon_policy|fixed_budget):' /proc/vhuge || true
fi

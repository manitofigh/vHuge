#!/bin/bash
set -euo pipefail

VHUGE_SERVICE="${VHUGE_SERVICE:-vhuge-poold.service}"
VHUGE_SYSFS="${VHUGE_SYSFS:-/sys/kernel/vhuge_pool}"

require_root() {
    if [[ ${EUID} -ne 0 ]]; then
        exec sudo "$0" "$@"
    fi
}

have_systemd() {
    command -v systemctl >/dev/null 2>&1
}

have_pool_sysfs() {
    [[ -d "${VHUGE_SYSFS}" ]]
}

write_knob() {
    local knob="$1"
    local value="$2"
    local path="${VHUGE_SYSFS}/${knob}"

    if [[ -e "${path}" ]]; then
        printf '%s\n' "${value}" > "${path}"
    fi
}

stop_service() {
    if have_systemd; then
        systemctl stop "${VHUGE_SERVICE}" || true
        systemctl reset-failed "${VHUGE_SERVICE}" || true
    else
        echo "systemctl not found; cannot stop ${VHUGE_SERVICE}" >&2
        return 1
    fi
}

start_service() {
    if have_systemd; then
        systemctl start "${VHUGE_SERVICE}"
    else
        echo "systemctl not found; cannot start ${VHUGE_SERVICE}" >&2
        return 1
    fi
}

clean_kernel_state() {
    if ! have_pool_sysfs; then
        echo "vHuge sysfs not present at ${VHUGE_SYSFS}; kernel pool is unavailable on this boot." >&2
        return 1
    fi

    write_knob enabled 0
    write_knob reset_stats 1
}

enable_clean() {
    clean_kernel_state
    write_knob enabled 1
}

print_status() {
    if have_pool_sysfs; then
        printf 'vHuge enabled='
        cat "${VHUGE_SYSFS}/enabled" 2>/dev/null || printf 'unknown\n'
        printf 'vHuge current='
        cat "${VHUGE_SYSFS}/current" 2>/dev/null || printf 'unknown\n'
    fi

    if have_systemd; then
        systemctl --no-pager --full status "${VHUGE_SERVICE}" || true
    fi
}

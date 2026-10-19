#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

POOL_DIR=/sys/kernel/vhuge_pool

wait_no_process()
{
    local pattern=$1
    local label=$2
    local i

    # pattern matches the process name, not the command line.
    for i in {1..100}; do
        if ! pgrep "${pattern}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    echo "${label} is still running after vHuge shutdown:" >&2
    pgrep -a "${pattern}" >&2 || true
    return 1
}

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop vhuge-poold.service >/dev/null 2>&1 || true
    systemctl reset-failed vhuge-poold.service >/dev/null 2>&1 || true
fi
pkill -TERM -x vhuge-poold >/dev/null 2>&1 || true

if [[ -d "${POOL_DIR}" ]]; then
    if [[ -w "${POOL_DIR}/damon_enabled" ]]; then
        printf '0\n' > "${POOL_DIR}/damon_enabled"
    fi
    if [[ -w "${POOL_DIR}/damon_policy_reset" ]]; then
        printf '1\n' > "${POOL_DIR}/damon_policy_reset"
    fi
    printf '0\n' > "${POOL_DIR}/enabled"
    printf '1\n' > "${POOL_DIR}/reset_stats"
fi

if [[ -w /sys/kernel/mm/transparent_hugepage/enabled ]]; then
    printf 'always\n' > /sys/kernel/mm/transparent_hugepage/enabled
fi

wait_no_process '^kdamond' 'kdamond'
wait_no_process '^vhuge-poold$' 'vhuge-poold'

if [[ -r /proc/vhuge ]]; then
    grep -q '^enabled: 0 ' /proc/vhuge || {
        echo "vHuge is not disabled after shutdown" >&2
        cat /proc/vhuge >&2
        exit 1
    }
    if [[ -e "${POOL_DIR}/damon_enabled" ]]; then
        grep -q '^damon: enabled=0 running=0 ' /proc/vhuge || {
            echo "vHuge DAMON is not fully disabled after shutdown" >&2
            cat /proc/vhuge >&2
            exit 1
        }
    fi
fi

#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    exec sudo bash "$0" "$@"
fi

POOL_DIR=/sys/kernel/vhuge_pool
WAIT_TIMEOUT_SEC=${VHUGE_WAIT_TIMEOUT_SEC:-600}

[[ -d "${POOL_DIR}" ]] || {
    echo "missing ${POOL_DIR}; boot the vHuge kernel first" >&2
    exit 1
}

systemctl list-unit-files vhuge-poold.service >/dev/null 2>&1 || {
    echo "vhuge-poold.service is not installed; run ./scripts/setup.sh daemon first" >&2
    exit 1
}

wait_for_daemon()
{
    local i

    for i in {1..100}; do
        if pgrep -x vhuge-poold >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    echo "vhuge-poold did not start" >&2
    return 1
}

if [[ -w /sys/kernel/mm/transparent_hugepage/enabled ]]; then
    printf 'always\n' > /sys/kernel/mm/transparent_hugepage/enabled
fi

printf '1\n' > "${POOL_DIR}/enabled"
printf '1\n' > "${POOL_DIR}/reset_stats"
if [[ -w "${POOL_DIR}/damon_enabled" ]]; then
    if [[ -w "${POOL_DIR}/damon_policy_reset" ]]; then
        printf '1\n' > "${POOL_DIR}/damon_policy_reset"
    fi
    printf '1\n' > "${POOL_DIR}/damon_enabled"
fi

systemctl start vhuge-poold.service

low=$(cat "${POOL_DIR}/low_watermark")
deadline=$((SECONDS + WAIT_TIMEOUT_SEC))
while (( SECONDS < deadline )); do
    current=$(cat "${POOL_DIR}/current")
    if (( current >= low )); then
        grep -q '^enabled: 1 ' /proc/vhuge || {
            echo "vHuge is not enabled after startup" >&2
            cat /proc/vhuge >&2
            exit 1
        }
        if [[ -w "${POOL_DIR}/damon_enabled" ]]; then
            grep -q '^damon: enabled=1 ' /proc/vhuge || {
                echo "vHuge DAMON policy is not enabled after startup" >&2
                cat /proc/vhuge >&2
                exit 1
            }
        fi
        wait_for_daemon
        exit 0
    fi
    sleep 1
done

echo "timed out waiting for vHuge pool: current=${current:-0} low=${low}" >&2
cat /proc/vhuge >&2 || true
exit 1

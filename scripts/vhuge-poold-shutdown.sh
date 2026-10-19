#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/vhuge-poold-common.sh
source "${SCRIPT_DIR}/vhuge-poold-common.sh"

require_root "$@"

echo "[+] Stopping ${VHUGE_SERVICE}"
stop_service

echo "[+] Disabling vHuge placement, draining the REAL pool, and resetting counters"
if ! clean_kernel_state; then
    echo "[!] Service stopped, but vHuge kernel sysfs was not available." >&2
    exit 1
fi

print_status

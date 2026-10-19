#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=scripts/vhuge-poold-common.sh
source "${SCRIPT_DIR}/vhuge-poold-common.sh"

require_root "$@"

echo "[+] Stopping ${VHUGE_SERVICE}"
stop_service

echo "[+] Draining the REAL pool and resetting counters"
enable_clean

echo "[+] Starting ${VHUGE_SERVICE}"
start_service

print_status

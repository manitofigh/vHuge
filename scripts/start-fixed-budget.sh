#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 || ! $1 =~ ^[0-9]+$ ]]; then
    echo "usage: $0 HOT_BUDGET_PAGES" >&2
    exit 2
fi

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

export VHUGE_DAMON_POLICY_MODE=1
export VHUGE_DAMON_HOT_BUDGET_PAGES=$1
export VHUGE_DAMON_HOT_THRESHOLD_BP=${VHUGE_DAMON_HOT_THRESHOLD_BP:-0}
exec "${SCRIPT_DIR}/start-damon.sh"

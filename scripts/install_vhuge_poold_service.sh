#!/bin/bash
set -euo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
KREL="${KREL:-$(uname -r)}"
MODULE_DIR="/lib/modules/${KREL}/extra"

if [[ ! -d "/lib/modules/${KREL}/build" ]]; then
    echo "Missing kernel build directory: /lib/modules/${KREL}/build" >&2
    echo "Install headers or finish installing the vHuge kernel build tree first." >&2
    exit 1
fi

# scripts/setup.sh daemon builds both as the invoking user before calling this
# script. Build here only when run standalone and a binary is missing or the
# module was built for another kernel.
PTEDITOR_KO="${ROOT_DIR}/misc/PTEditor/module/pteditor.ko"

if [[ ! -x "${BUILD_DIR}/vhuge-poold" ]]; then
    echo "[+] Building vhuge-poold"
    cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}"
    cmake --build "${BUILD_DIR}" -j"$(nproc)" --target vhuge-poold
fi

if [[ ! -f "${PTEDITOR_KO}" ]] ||
   [[ "$(modinfo -F vermagic "${PTEDITOR_KO}" | cut -d' ' -f1)" != "${KREL}" ]]; then
    echo "[+] Building PTEditor for ${KREL}"
    make -C "${ROOT_DIR}/misc/PTEditor/module" clean KERNEL="${KREL}"
    make -C "${ROOT_DIR}/misc/PTEditor/module" KERNEL="${KREL}"
fi

echo "[+] Installing binaries and module"
install -D -m 0755 "${BUILD_DIR}/vhuge-poold" /usr/local/sbin/vhuge-poold
install -D -m 0755 "${ROOT_DIR}/scripts/vhuge-load-pteditor.sh" /usr/local/sbin/vhuge-load-pteditor
install -d -m 0755 "${MODULE_DIR}"
install -m 0644 "${PTEDITOR_KO}" "${MODULE_DIR}/pteditor.ko"
depmod -a "${KREL}"

echo "[+] Installing systemd unit"
install -D -m 0644 "${ROOT_DIR}/scripts/vhuge-poold.service" /etc/systemd/system/vhuge-poold.service
systemctl daemon-reload
systemctl enable vhuge-poold.service

echo "[+] Starting vhuge-poold"
systemctl restart vhuge-poold.service
systemctl --no-pager --full status vhuge-poold.service

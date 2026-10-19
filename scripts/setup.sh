#!/usr/bin/env bash
# Build and install vHuge inside a VM.
#
#   ./scripts/setup.sh deps      install build dependencies (apt)
#   ./scripts/setup.sh user      build the userspace tools into build/
#   ./scripts/setup.sh kernel    fetch Linux 6.15.2, add the vHuge changes, build and install it
#   ./scripts/setup.sh daemon    install PTEditor and the vhuge-poold service (run on the vHuge kernel)
#   ./scripts/setup.sh all       deps + user, then build and load PTEditor (stock or vHuge kernel)
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
KVER=6.15.2
KDIR="${ROOT}/linux-${KVER}"
KURL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-${KVER}.tar.xz"

deps()
{
    sudo apt-get update
    sudo apt-get install -y build-essential cmake bc bison flex dwarves \
        libssl-dev libelf-dev libncurses-dev \
        numactl wget xz-utils
    # PTEditor builds against the running kernel; the vHuge kernel brings its own tree.
    if apt-cache show "linux-headers-$(uname -r)" >/dev/null 2>&1; then
        sudo apt-get install -y "linux-headers-$(uname -r)"
    fi
}

user()
{
    cmake -S "${ROOT}" -B "${ROOT}/build"
    cmake --build "${ROOT}/build" -j"$(nproc)"
    echo "userspace tools are in ${ROOT}/build"
}

all()
{
    deps
    user
    make -C "${ROOT}/misc/PTEditor/module"
    if ! lsmod | grep -qw pteditor; then
        sudo insmod "${ROOT}/misc/PTEditor/module/pteditor.ko"
    fi
    echo "run: sudo ${ROOT}/build/vhuge tlb -n 256"
}

kernel()
{
    # The repository tracks only the files that vHuge changes. Unpack the
    # rest of the kernel tree around them without overwriting them.
    if [[ ! -f ${KDIR}/Makefile ]]; then
        wget -c -O "/tmp/linux-${KVER}.tar.xz" "${KURL}"
        tar -xf "/tmp/linux-${KVER}.tar.xz" -C "${KDIR}" \
            --strip-components=1 --skip-old-files
    fi
    cd "${KDIR}"
    [[ -f .config ]] || cp "/boot/config-$(uname -r)" .config
    # Automatic NUMA balancing would move pages off the real huge pages
    # that vHuge placed them on.
    scripts/config --enable TRANSPARENT_HUGEPAGE --enable DAMON \
        --enable DAMON_VADDR --enable VHUGE_KM --enable VHUGE_DAMON \
        --disable NUMA_BALANCING \
        --set-str SYSTEM_TRUSTED_KEYS "" --set-str SYSTEM_REVOCATION_KEYS ""
    make olddefconfig
    make -j"$(nproc)"
    sudo make INSTALL_MOD_STRIP=1 modules_install
    sudo make install
    echo "reboot into ${KVER}-vhuge, then run: ./scripts/setup.sh daemon"
}

daemon()
{
    [[ -d /sys/kernel/vhuge_pool ]] || {
        echo "not running the vHuge kernel (uname -r: $(uname -r))" >&2
        exit 1
    }
    # Build as the invoking user so build/ and misc/PTEditor/module stay
    # user-owned; the installer then only installs.
    cmake -S "${ROOT}" -B "${ROOT}/build"
    cmake --build "${ROOT}/build" -j"$(nproc)" --target vhuge-poold
    make -C "${ROOT}/misc/PTEditor/module" clean
    make -C "${ROOT}/misc/PTEditor/module"
    sudo "${ROOT}/scripts/install_vhuge_poold_service.sh"
}

case "${1:-}" in
    deps|user|kernel|daemon|all) "$1" ;;
    *) sed -n '2,8p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac

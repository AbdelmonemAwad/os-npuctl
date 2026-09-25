#!/bin/sh
# Install from a copy of this repository, without pkg(8).
#
# This installs the RESET HOOK and the TOOLS. It does not build or load the kernel module - see
# docs/porting-notes.md for why that is a separate, deliberate step.
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-/usr/local}

if [ "$(id -u)" != "0" ]; then
    echo "run this as root"
    exit 1
fi

echo "== tools =="
install -d -m 0755 "${PREFIX}/opnsense/scripts/npuctl"
install -m 0755 "${SRC}/src/opnsense/scripts/npuctl/mcp2210.py" "${PREFIX}/opnsense/scripts/npuctl/"
install -m 0755 "${SRC}/src/opnsense/scripts/npuctl/npuhs.py" "${PREFIX}/opnsense/scripts/npuctl/"

echo "== boot hook =="
install -d -m 0755 "${PREFIX}/etc/rc.syshook.d/early"
install -m 0755 "${SRC}/src/etc/rc.syshook.d/early/01-npuctl" "${PREFIX}/etc/rc.syshook.d/early/01-npuctl"

# hidraw(4) is what gives a device node for the USB-SPI bridge. hidbus attaches on its own but
# leaves no node until a child driver claims it, and without that node nothing can reach the
# reset line. Loading it from loader.conf rather than from the hook keeps it available even if
# the hook is later removed.
echo "== loader =="
if ! grep -q '^hidraw_load=' /boot/loader.conf.local 2>/dev/null; then
    printf 'hidraw_load=%cYES%c\n' 34 34 >> /boot/loader.conf.local
    echo "   added hidraw_load to /boot/loader.conf.local"
else
    echo "   hidraw_load already present"
fi
kldstat -q -m hidraw || kldload hidraw 2>/dev/null || true

echo
echo "== installed =="
echo "   The hook runs at the next boot. To release the NPU now, without rebooting:"
echo "     ${PREFIX}/etc/rc.syshook.d/early/01-npuctl"
echo
echo "   To see what the bridge reports, changing nothing:"
echo "     python3 ${PREFIX}/opnsense/scripts/npuctl/mcp2210.py status"

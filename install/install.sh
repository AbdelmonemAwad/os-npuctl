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
install -m 0755 "${SRC}/install/verify.sh" "${PREFIX}/opnsense/scripts/npuctl/verify.sh"

echo "== boot hook =="
install -d -m 0755 "${PREFIX}/etc/rc.syshook.d/early"
install -m 0755 "${SRC}/src/etc/rc.syshook.d/early/01-npuctl" "${PREFIX}/etc/rc.syshook.d/early/01-npuctl"
install -m 0755 "${SRC}/src/etc/rc.syshook.d/early/02-npuep" "${PREFIX}/etc/rc.syshook.d/early/02-npuep"

# And the module itself, if one has been built. It is not packaged - it is C against this kernel's
# headers, so it is built on the appliance - but a built module that is never installed is a module
# somebody has to remember to load by hand, and forgetting is what makes a firewall come up without
# its ports. /boot/modules is where 02-npuep looks.
echo "== kernel module =="
KO=""
for c in "${SRC}/contrib/npuep/npuep.ko" /root/npu/kmod/npuep.ko; do
    if [ -f "${c}" ]; then
        KO="${c}"
        break
    fi
done
if [ -n "${KO}" ]; then
    install -d -m 0755 /boot/modules
    install -m 0555 "${KO}" /boot/modules/npuep.ko
    echo "   installed ${KO} as /boot/modules/npuep.ko"
else
    echo "   none built yet - see contrib/npuep/build.sh; the boot hook will skip until there is one"
fi

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
echo "   Both hooks run at the next boot: 01 releases the NPU from reset, 02 loads the driver"
echo "   and waits for the front ports so that OPNsense's interface configuration can see them."
echo
echo "   To release the NPU now, without rebooting:"
echo "     ${PREFIX}/etc/rc.syshook.d/early/01-npuctl"
echo
echo "   To see what the bridge reports, changing nothing:"
echo "     python3 ${PREFIX}/opnsense/scripts/npuctl/mcp2210.py status"

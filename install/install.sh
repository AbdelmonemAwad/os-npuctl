#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
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
install -m 0755 "${SRC}/src/opnsense/scripts/npuctl/bridge-pathcost.sh" \
    "${PREFIX}/opnsense/scripts/npuctl/bridge-pathcost.sh"

echo "== boot hook =="
install -d -m 0755 "${PREFIX}/etc/rc.syshook.d/early"
install -m 0755 "${SRC}/src/etc/rc.syshook.d/early/06-npuctl" "${PREFIX}/etc/rc.syshook.d/early/06-npuctl"
install -m 0755 "${SRC}/src/etc/rc.syshook.d/early/07-npuep" "${PREFIX}/etc/rc.syshook.d/early/07-npuep"

# devd fires this when a front port's link comes up, which is the only moment RSTP will accept a
# path cost for it. See src/opnsense/scripts/npuctl/bridge-pathcost.sh for why that matters.
#
# A timer, not a link-up hook, and the reason is in bridge-pathcost.sh: devd executes only the
# single highest-priority statement that matches an event, and OPNsense already claims
# IFNET/LINK_UP at priority 101 to run its own linkup handling. Below that a rule never fires;
# above it, ours would fire and silence OPNsense's. Neither is acceptable, so this runs on cron.
#
# /usr/local/etc/cron.d, not /etc/crontab - OPNsense regenerates the latter from its own
# configuration and would drop the line.
echo "== path cost timer =="
install -d -m 0755 "${PREFIX}/etc/cron.d"
install -m 0644 "${SRC}/src/etc/cron.d/npuctl" "${PREFIX}/etc/cron.d/npuctl"
echo "   installed ${PREFIX}/etc/cron.d/npuctl (once a minute)"

# And once now, so an install does not have to wait for the first tick.
if [ -x "${PREFIX}/opnsense/scripts/npuctl/bridge-pathcost.sh" ]; then
    "${PREFIX}/opnsense/scripts/npuctl/bridge-pathcost.sh" || true
fi

# And the module itself, if one has been built. It is not packaged - it is C against this kernel's
# headers, so it is built on the appliance - but a built module that is never installed is a module
# somebody has to remember to load by hand, and forgetting is what makes a firewall come up without
# its ports. /boot/modules is where 07-npuep looks.
echo "== kernel module =="
# The appliance's own build directory comes FIRST, and the order is deliberate. A .ko is only
# valid for the kernel it was compiled against, /root/npu/kmod is where it is compiled on the
# machine it will run on, and a checkout can carry a stale one from anywhere. Searching the
# checkout first meant a left-over file won silently and the next boot loaded the wrong build.
KO=""
for c in /root/npu/kmod/npuep.ko "${SRC}/contrib/npuep/npuep.ko"; do
    if [ -f "${c}" ]; then
        KO="${c}"
        break
    fi
done
if [ -n "${KO}" ]; then
    install -d -m 0755 /boot/modules
    install -m 0555 "${KO}" /boot/modules/npuep.ko
    echo "   installed ${KO} as /boot/modules/npuep.ko"
    # Carry the build's kernel stamp with it, and say so now rather than at the next boot.
    if [ -f "${KO}.kernel" ]; then
        install -m 0444 "${KO}.kernel" /boot/modules/npuep.ko.kernel
        if [ "$(cat "${KO}.kernel")" != "$(uname -v)" ]; then
            echo "   WARNING: built against a different kernel than the one running."
            echo "            built  : $(cat "${KO}.kernel")"
            echo "            running: $(uname -v)"
            echo "            Rebuild before rebooting: sh contrib/npuep/build.sh"
        fi
    else
        rm -f /boot/modules/npuep.ko.kernel
        echo "   no kernel stamp beside it - verify.sh cannot tell whether it matches"
    fi
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
echo "     ${PREFIX}/etc/rc.syshook.d/early/06-npuctl"
echo
echo "   To see what the bridge reports, changing nothing:"
echo "     python3 ${PREFIX}/opnsense/scripts/npuctl/mcp2210.py status"
echo
echo "   After reloading the module by hand, the front ports' ifnets are new, so a bridge over"
echo "   them has to be rebuilt. The path costs follow within a minute on their own:"
echo "     configctl interface bridge configure"
echo "     ${PREFIX}/opnsense/scripts/npuctl/bridge-pathcost.sh   # or just wait"

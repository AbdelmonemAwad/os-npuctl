#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Reset the coprocessor, swap the module, and report. Run: sh /root/npu/kmod/reload.sh
#
# The reset before unloading is not tidiness. Once the management interface has published ring
# addresses, the coprocessor is writing into host memory; the detach path withdraws first and
# waits, but a reset is the only thing that makes it certain. Doing it here also puts the far
# side back to a known state for the next attach.
#
# WAIT FOR THE ENDPOINT, DO NOT COUNT SECONDS. This script used to sleep 25 seconds and load,
# on the reasoning that userspace cannot see whether the far side is back: the reset clears the
# endpoint BARs, FreeBSD keeps serving its cached addresses, and reads through /dev/mem come
# back as all ones however healthy the coprocessor is. That is true of MEMORY space and it is
# not true of CONFIG space, which is what this now polls - `pciconf -r` reads it live, and a
# device that is not answering reads 0xffff there rather than hanging.
#
# The distinction is worth a paragraph because getting it wrong stops the machine. A read of a
# BAR belonging to an endpoint whose link is down does not fail and does not time out: it never
# completes, and the host dies with no panic and no console output. Measured here, at the cost
# of a power cycle, after a pulse and a FORTY-FIVE second wait - longer than the twenty-five
# this script used, which is the point. The wait was never too short. It was blind.
set -e
CTL=/usr/local/opnsense/scripts/npuctl
D=$(dirname "$0")
: ${WAIT:=90}

# The endpoint, by vendor and device rather than by a hard-coded path, because the bus number
# is not ours to assume.
#
# Parse it with awk's DEFAULT field separator. An earlier version passed -F'[@ ]', which does
# not include the tab that pciconf actually puts between the selector and the class - so the
# field came out as "pci0:1:0:0:<tab>class=0x020000", pciconf refused to parse it, every probe
# fell back to the all-ones default, and the script waited the full ninety seconds and blamed
# the hardware for a bug in this line. A dead device and an unparseable selector are not the
# same thing and must not look the same, which is what the shape check below is for.
SEL=$(pciconf -l 2>/dev/null | awk '/chip=0x708011ab|vendor=0x11ab/ {
    s = $1
    sub(/^[^@]*@/, "", s)   # drop the driver name and the @
    sub(/:$/, "", s)        # drop the trailing colon pciconf prints
    print s
    exit
}')
[ -n "${SEL}" ] || SEL=pci0:1:0:0

case "${SEL}" in
    pci[0-9]*:[0-9]*:[0-9]*:[0-9]*)
        ;;
    *)
        echo "cannot work out the endpoint's selector - got '${SEL}'." >&2
        echo "pciconf -l output has changed shape; fix the awk above rather than waiting." >&2
        exit 2
        ;;
esac

if kldstat -q -n npuep; then
    echo "== unloading =="
    kldunload npuep || true
fi

echo "== resetting the NPU =="
python3 "${CTL}/mcp2210.py" pulse || true

echo "== waiting for ${SEL} to answer config space (up to ${WAIT}s) =="
i=0
answered=no
while [ "${i}" -lt "${WAIT}" ]; do
    v=$(pciconf -r "${SEL}" 0:1 2>/dev/null || echo ffffffff)
    case "${v}" in
        ""|*ffffffff*|*ffff)
            ;;
        *)
            echo "   answering after ${i}s: ${v}"
            answered=yes
            break
            ;;
    esac
    i=$((i + 1))
    sleep 1
done

if [ "${answered}" != yes ]; then
    # Refusing is the right answer. Loading now is what hangs the host, and a hung host is worse
    # than a driver that did not load: one of them you can still log into.
    echo "   the endpoint never answered. NOT loading - a read of its memory in this state"
    echo "   hangs the machine with no panic and no log. Power cycle and try again."
    exit 1
fi

# It answers config space, which means the link is up. That is not the same as its Linux being
# ready, and the module's own DEV_READY wait covers the rest - but it is now waiting on a device
# that can be read rather than on one that might stop the host.
echo "== loading =="
kldload "${D}/npuep.ko"
sleep 3
echo
dmesg | tail -20

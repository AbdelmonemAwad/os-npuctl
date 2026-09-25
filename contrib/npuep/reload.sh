#!/bin/sh
# Reset the coprocessor, swap the module, and report. Run: sh /root/npu/kmod/reload.sh
#
# The reset before unloading is not tidiness. Once the management interface has published ring
# addresses, the coprocessor is writing into host memory; the detach path withdraws first and
# waits, but a reset is the only thing that makes it certain. Doing it here also puts the far
# side back to a known state for the next attach.
set -e
CTL=/usr/local/opnsense/scripts/npuctl
D=$(dirname "$0")

if kldstat -q -n npuep; then
    echo "== unloading =="
    kldunload npuep || true
fi

echo "== resetting the NPU =="
python3 "${CTL}/mcp2210.py" pulse || true

# Wait for the coprocessor to boot. This cannot be checked with npuhs.py first, and the reason
# is worth knowing: the reset clears the endpoint BARs, FreeBSD keeps serving its cached
# addresses, and userspace reads through /dev/mem come back as all ones no matter how healthy
# the far side is. The BARs are restored by pci_cfg_restore on the kldload path - so the module
# is the first thing that can see anything. It reports clearly if the NPU is not ready.
echo "== waiting 25s for the NPU to boot =="
sleep 25

echo "== loading =="
kldload "${D}/npuep.ko"
sleep 3
echo
dmesg | tail -20

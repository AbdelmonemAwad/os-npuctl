#!/bin/sh
# Remove everything this repository installs.
#
# It does not put the coprocessor back into reset. Leaving a running NPU running is the less
# surprising of the two options, and a power cycle restores the factory state anyway - the
# release is written to the bridge's volatile settings and never to its NVRAM.
set -e

PREFIX=${PREFIX:-/usr/local}

if [ "$(id -u)" != "0" ]; then
    echo "run this as root"
    exit 1
fi

if kldstat -q -n npuep; then
    echo "== npuep is loaded; unload it yourself when you are ready =="
    echo "   kldunload npuep"
    echo "   (pulse the NPU's reset first - it raises doorbells by writing the MSI message"
    echo "    itself, so freed vectors mean its writes land on whatever replaces them)"
fi

rm -f "${PREFIX}/etc/rc.syshook.d/early/01-npuctl"
rm -f "${PREFIX}/opnsense/scripts/npuctl/mcp2210.py"
rm -f "${PREFIX}/opnsense/scripts/npuctl/npuhs.py"
rmdir "${PREFIX}/opnsense/scripts/npuctl" 2>/dev/null || true

# hidraw_load is left in loader.conf.local on purpose: it is harmless, it may have been there
# before this plugin, and removing a line somebody else added is worse than leaving one behind.
echo "removed. hidraw_load was left in /boot/loader.conf.local."

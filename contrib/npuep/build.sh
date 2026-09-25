#!/bin/sh
# Build the module on the appliance. Run from anywhere:  sh /root/npu/kmod/build.sh
#
# Separate from the Makefile so it can be invoked over ssh without three layers of quoting
# between PowerShell, sh and make eating the redirections.
set -e
cd "$(dirname "$0")"
rm -f *.o *.ko
if make SYSDIR=/usr/src/sys > /tmp/build.log 2>&1; then
    echo "BUILD OK"
    ls -l npuep.ko
else
    echo "BUILD FAILED"
    grep -E 'error:|warning:' /tmp/build.log | head -30
    tail -5 /tmp/build.log
    exit 1
fi

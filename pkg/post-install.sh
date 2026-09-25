#!/bin/sh
# Run by pkg(8) as root after the files are in place.
set -e

if ! grep -q '^hidraw_load=' /boot/loader.conf.local 2>/dev/null; then
    printf 'hidraw_load=%cYES%c\n' 34 34 >> /boot/loader.conf.local
fi
kldstat -q -m hidraw || kldload hidraw 2>/dev/null || true

# Deliberately does NOT release the NPU here. An install should not change what the hardware is
# doing until the next boot, when the hook runs in a context where its output is logged and the
# machine is in a known state.
exit 0

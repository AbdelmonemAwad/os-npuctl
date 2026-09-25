#!/bin/sh
# Can a packet cross? The coprocessor gives its end of the link fe80::0002.
set -e
ifconfig mvmgmt0 inet6 -ifdisabled || true
sleep 2
echo "--- interface ---"
ifconfig mvmgmt0
echo "--- neighbours ---"
ndp -an 2>/dev/null | grep -i mvmgmt || echo "  (none yet)"
echo "--- ping the coprocessor ---"
ping6 -c 4 -W 2000 "fe80::2%mvmgmt0" 2>&1 | tail -8 || true
echo "--- counters ---"
netstat -I mvmgmt0 -b 2>/dev/null | head -3
sysctl -n dev.npuep.0.mvmgmt_db0 2>/dev/null

#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Map every front port to the connector it really is, by transmitting out of each one and seeing
# which one hears it. Run with loopback cables between pairs of front ports:
#
#	sh contrib/npuep/portmap.sh
#
# What it proves, per port, in one pass:
#
#   - that a frame written to npupN really leaves the physical connector, which nothing else here
#     can show. tcpdump on the sending interface is a BPF tap taken before the frame is handed to
#     the coprocessor, so it says the driver transmitted, not that anything came out;
#   - that the frame arrives with the tag of the port it came IN on, and only that tag;
#   - that the internal switch does not forward between front ports behind the host's back - the
#     sending port's own counter must not move.
#
# The probe is an ARP request for an address nobody owns, in a /24 belonging to that port alone,
# so a frame heard on another port is unambiguous about where it came from. No cooperating device
# is needed at the far end, which is the whole point: a peer that will not answer ARP proves
# nothing either way, and one that answers proves only that one path works.
#
# PortF1 and PortF2 (npup13, npup14) are the SFP cages. A copper patch lead will not loop them.
set -u

PORTS="1 2 3 4 5 6 7 8 9 10 11 12 13 14"

# The driver's counters name the fibre ports F1 and F2 rather than 13 and 14.
pname() {
	case "$1" in
	13)	echo F1 ;;
	14)	echo F2 ;;
	*)	echo "$1" ;;
	esac
}

ctr() {
	sysctl -n "dev.npuep.0.giu.rx_port.Port$(pname "$1")" 2>/dev/null || echo 0
}

if ! sysctl -n dev.npuep.0.giu.rx_port.Port1 > /dev/null 2>&1; then
	echo "the driver is not loaded, or it has no per-port counters." >&2
	exit 1
fi

echo "== bringing all fourteen up =="
for i in ${PORTS}; do
	ifconfig "npup${i}" up 2>/dev/null
done
sleep 2

echo
echo "== which port hears each port =="
printf '  %-9s %s\n' "sent on" "heard on"
for i in ${PORTS}; do
	# Its own /24, so the probe names its source beyond argument.
	ifconfig "npup${i}" inet "10.99.${i}.1/24" alias 2>/dev/null
	sleep 1					# let the duplicate-address probe go first
	for j in ${PORTS}; do
		eval "B${j}=$(ctr "${j}")"
	done

	ping -c 2 -t 1 "10.99.${i}.77" > /dev/null 2>&1
	sleep 1

	heard=""
	for j in ${PORTS}; do
		eval "before=\$B${j}"
		after=$(ctr "${j}")
		d=$((after - before))
		if [ "${d}" -gt 0 ]; then
			heard="${heard} npup${j}(+${d})"
		fi
	done
	ifconfig "npup${i}" -alias "10.99.${i}.1" 2>/dev/null

	printf '  npup%-5s %s\n' "${i}" "${heard:-- nothing -}"
done

echo
echo "== every counter, after the sweep =="
for i in ${PORTS}; do
	printf '  Port%-4s %s\n' "$(pname "${i}")" "$(ctr "${i}")"
done

echo
echo "Read it like this. A line \"npup3  npup4(+2)\" means a frame written to npup3 left the"
echo "connector, crossed your cable, came back in on the connector the driver calls Port4, and was"
echo "tagged as Port4 - so both ports and both tags are right. \"- nothing -\" means that port has"
echo "no loopback cable, or that its egress does not reach the connector."

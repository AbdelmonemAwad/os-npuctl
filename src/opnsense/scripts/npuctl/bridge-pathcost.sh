#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Give a bridged front port the RSTP path cost its link speed deserves.
#
# WHY THIS EXISTS AT ALL, because a script that adjusts spanning tree by hand deserves a reason.
#
# if_bridge computes a member's path cost exactly once, in bstp_create, at the moment the member is
# added, and recomputes it at most one more time - only if the link was DOWN then, and only on the
# first carrier it sees afterwards. See bstp_calc_path_cost and BSTP_PORT_PNDCOST.
#
# These ports cannot satisfy that. OPNsense adds its bridge members seconds after npuep loads, the
# coprocessor's network agent does not publish its mailbox for about fourteen, and when carrier
# finally arrives the switch spends the first three seconds reporting 10baseT while it negotiates a
# gigabit link. Measured, once a second, from module load:
#
#	t+9	10baseT/UTP	<- the one moment RSTP looks
#	t+11	10baseT/UTP
#	t+12	none		<- the PHY renegotiates
#	t+15	1000baseT	<- settled, and what it stays
#
# So RSTP spends its single recalculation on 10 Mbit/s and latches 2000000 where the 802.1D formula
# gives 20000 for the link that is actually running. The driver cannot reach this: by the time it
# knows the truth, nothing will ask again. Userland can, with ifpathcost, which is what this does.
#
# It only matters on a bridge with a redundant path, where a hundredfold error in one port's cost
# elects the wrong root port. On a bridge without one it changes nothing, and it is written to be
# safe to run anyway: it reads, compares, and writes only what differs.
#
# WHY CRON AND NOT devd. The obvious hook is IFNET/LINK_UP, and it cannot be used. devd runs only
# ONE matching statement per event - "if two statements match the same event, only the action of
# the statement with highest priority will be executed", devd.conf(5) - and OPNsense already claims
# that event at priority 101 to run `configctl interface linkup start`. A rule below that never
# fires, which is what a rule at 50 did here: measured, nothing happened. A rule above it would
# fire and SILENCE OPNsense's own linkup handling for these ports, which is not a trade worth
# making on a firewall. So this runs on a timer instead, and is built to be cheap enough for one.
#
#	bridge-pathcost.sh		 every bridge, every npup member
#	bridge-pathcost.sh npup1	 only the bridge npup1 belongs to
#
set -u

ONLY=${1:-}

# ONE ifconfig for the whole decision, because this runs once a minute.
#
# The obvious shape - a call per bridge and a call per member - is a dozen processes a minute for a
# job that changes nothing on almost every run. Everything needed is in `ifconfig -a`: each port's
# media and status in its own block, each bridge's members and their current costs in the bridge's.
# So it is read once and the answer worked out in a single awk.
#
# The awk prints one line per member that needs changing, and nothing at all when none do, which is
# the normal case. The 802.1D-2004 section 17.14 figure is the same one bstp_calc_path_cost uses:
#
#	20000000000 / (baudrate / 1000)   ==   20000000 / megabits
#
work=$(ifconfig -a 2>/dev/null | awk -v only="${ONLY}" '
	/^[a-z][a-z0-9.]*:/ {
		cur = substr($1, 1, length($1) - 1)
		next
	}
	/^\tstatus:/ { status[cur] = $2; next }
	/^\tmedia:/  { media[cur]  = $0;  next }
	/^\tmember:/ {
		n++
		br[n] = cur
		mb[n] = $2
		next
	}
	/path cost/ {
		if (n > 0 && cost[n] == "")
			for (i = 1; i < NF; i++)
				if ($i == "cost") { cost[n] = $(i + 1); break }
		next
	}
	END {
		# If an interface was named, work out which bridge holds it and do only that one.
		if (only != "") {
			for (i = 1; i <= n; i++)
				if (mb[i] == only)
					wanted_br = br[i]
			if (wanted_br == "")
				exit 0
		}

		for (i = 1; i <= n; i++) {
			if (wanted_br != "" && br[i] != wanted_br)
				continue

			m = mb[i]

			# Only the ports belonging to this driver. Every other NIC on this appliance
			# reports its speed before OPNsense gets round to bridging it, so the figure
			# FreeBSD worked out is already right and meddling would be worse.
			if (m !~ /^npup[0-9]+$/)
				continue

			# A dark port is deliberately left alone. Setting a cost sets
			# BSTP_PORT_ADMCOST, which tells RSTP never to compute one again - so pinning
			# a guess for a port with no cable would outlast the guess. It keeps the
			# neutral default until a cable arrives, and this runs again a minute later.
			if (status[m] != "active")
				continue

			# Longest first: 2500baseT before 1000baseT before 100baseTX before 10baseT.
			s = 0
			if      (media[m] ~ /2500base/) s = 2500
			else if (media[m] ~ /1000base/) s = 1000
			else if (media[m] ~ /100base/)  s = 100
			else if (media[m] ~ /10base/)   s = 10
			if (s == 0)
				continue

			want = int(20000000 / s)
			if (cost[i] == want "")
				continue

			print br[i], m, want, s, (cost[i] == "" ? "unset" : cost[i])
		}
	}')

[ -n "${work}" ] || exit 0

echo "${work}" | while read -r br m want speed had; do
	# daemon.notice, which is what 06-npuctl and 07-npuep already use, and NOT daemon.info.
	#
	# The collector on this box keeps notice and above and drops info. Measured, by sending one
	# message at each of info, notice and warn and grepping /var/log for all three: notice and
	# warn arrived in system_<date>.log, info went nowhere. A first version of this logged at
	# daemon.info and was silent for exactly that reason - which looked like the script not
	# running.
	if ifconfig "${br}" ifpathcost "${m}" "${want}" 2>/dev/null; then
		logger -t npuctl -p daemon.notice \
		    "${br}: ${m} path cost ${had} -> ${want} (${speed} Mb/s)"
	else
		logger -t npuctl -p daemon.warn \
		    "${br}: could not set ${m} path cost to ${want}"
	fi
done

exit 0

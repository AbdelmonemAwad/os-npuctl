#!/bin/sh
# Fetch the kernel sources that match THIS running kernel, and print the SYSDIR to build against.
#
#	sh contrib/npuep/fetch-sources.sh
#	SYSDIR=/usr/src-26.7-<sha>/sys sh contrib/npuep/build.sh
#
# Why this exists at all. An out-of-tree kernel module is compiled against headers, and the
# headers have to be the ones the running kernel was built from. OPNsense does not ship kernel
# sources and there is no package that provides them, so for a long time this appliance simply
# had a 333MB /usr/src/sys that somebody had copied there once - no version recorded, no
# provenance, not a package and not a checkout. It built, so nobody asked.
#
# It was the wrong tree. It was stock FreeBSD 15.1-RELEASE, BRANCH="RELEASE", while the kernel
# is OPNsense's own build of 15.1-RELEASE-p1 from github.com/opnsense/src. The trees differ in
# 156 files. That the module worked anyway was luck, and it was only confirmed as luck by
# fetching the right tree and rebuilding: __FreeBSD_version is 1501000 in both, none of the 156
# files is in a path this driver includes, and the two builds came out BYTE-IDENTICAL. Good
# news, and not a reason to keep guessing - the next kernel is not obliged to be so kind.
#
# The kernel names its own commit. `uname -v` on this board reads
#
#	FreeBSD 15.1-RELEASE-p1 stable/26.7-n283674-12334a596709 SMP
#
# and the last field of that middle token is the git commit in opnsense/src. So the sources can
# always be pinned to exactly what is running, with no version file to keep up to date.
#
# ORDER MATTERS AFTER AN UPDATE. `uname -v` reports the RUNNING kernel, so run this after the
# reboot that brings the new kernel up, not before - beforehand it pins the old one perfectly
# and uselessly.
set -e

SYS_ID=$(uname -v | awk '{for (i = 1; i <= NF; i++) if ($i ~ /^stable\//) print $i}')
if [ -z "${SYS_ID}" ]; then
	echo "cannot find a source revision in: $(uname -v)" >&2
	echo "this does not look like an OPNsense kernel; build against /usr/src/sys." >&2
	exit 1
fi

SHA=${SYS_ID##*-}
BRANCH=${SYS_ID%%-*}			# e.g. stable/26.7
SERIES=${BRANCH#stable/}
DST=${DST:-/usr/src-${SERIES}-${SHA}}
URL="https://codeload.github.com/opnsense/src/tar.gz/${SHA}"

echo "running kernel : $(uname -v)"
echo "source commit  : ${SHA} on ${BRANCH}"
echo "destination    : ${DST}"

if [ -d "${DST}/sys" ]; then
	echo "already there - nothing to fetch."
	echo
	echo "SYSDIR=${DST}/sys"
	exit 0
fi

# Only sys/ is extracted. The whole tarball still has to come down the wire - codeload has no way
# to ask for a subtree - but a module build needs nothing else: bsd.kmod.mk and the rest of the
# angle-bracket makefiles live in /usr/share/mk, which belongs to the base system and is already
# present and already the right version.
echo
echo "fetching (the whole tree comes down, only sys/ is written)..."
rm -rf "${DST}.partial"
mkdir -p "${DST}.partial"
if ! fetch -o - "${URL}" | tar -xz -C "${DST}.partial" --strip-components=1 '*/sys/*'; then
	rm -rf "${DST}.partial"
	echo "fetch or extract failed. Is ${SHA} reachable at ${URL} ?" >&2
	exit 1
fi

if [ ! -f "${DST}.partial/sys/sys/param.h" ]; then
	rm -rf "${DST}.partial"
	echo "extracted, but there is no sys/sys/param.h - the archive is not what was expected." >&2
	exit 1
fi
mv "${DST}.partial" "${DST}"

echo
echo "fetched $(du -sh "${DST}" | awk '{print $1}')"
echo "  tree says      : $(awk -F'"' '/^REVISION=/{r=$2} /^BRANCH=/{b=$2} END{print r "-" b}' "${DST}/sys/conf/newvers.sh")"
echo "  __FreeBSD_version: $(awk '/^#define[ \t]+__FreeBSD_version/{print $3}' "${DST}/sys/sys/param.h")"
echo "  running kernel   : $(sysctl -n kern.osreldate)"
echo
echo "Now build against it:"
echo "  SYSDIR=${DST}/sys sh contrib/npuep/build.sh"

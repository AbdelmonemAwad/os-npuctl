#!/bin/sh
# Check, on the appliance, that the whole chain still works.
#
# Run this after every OPNsense or FreeBSD update, and after anything that touches the boot path.
# It is the answer to the one question CI cannot ask: a GitHub runner has no Sophos appliance, no
# USB-SPI bridge and no coprocessor, so a green tick upstream says nothing about whether this
# machine still has fourteen front ports.
#
# It changes NOTHING. Every check reads. It is safe on a running firewall and safe to run twice.
#
# THE FAILURE THIS EXISTS FOR: the kernel module is built against one kernel's headers. An update
# that moves FreeBSD forward leaves a module that will not load, and the appliance then boots to a
# firewall with no front ports and no obvious reason why - the hook exits 0 by design, because a
# firewall that boots late is better than one that does not boot. So the silence is deliberate,
# and this is what breaks it.
#
#   sh /usr/local/opnsense/scripts/npuctl/verify.sh
#   sh install/verify.sh            (from a checkout)
#
# Exit 0 if everything a firewall needs is true, 1 otherwise.

pass=0
fail=0
warn=0

ok()   { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
note() { warn=$((warn + 1)); printf '  note  %s\n' "$1"; }

echo "== the appliance =="
printf '  %s\n' "$(uname -sr)"
printf '  %s\n' "$(opnsense-version 2>/dev/null || echo 'opnsense-version unavailable')"
printf '  board: %s / %s\n' "$(kenv smbios.planar.maker 2>/dev/null)" "$(kenv smbios.planar.product 2>/dev/null)"

echo
echo "== what is installed =="
for f in /usr/local/etc/rc.syshook.d/early/06-npuctl /usr/local/etc/rc.syshook.d/early/07-npuep; do
    if [ -x "$f" ]; then ok "$(basename $f) present and executable"; else bad "$f missing or not executable"; fi
done
if [ -f /boot/modules/npuep.ko ]; then ok "the module is installed in /boot/modules"; else bad "/boot/modules/npuep.ko missing - the boot hook will skip and there will be no front ports"; fi

echo
echo "== does the module match this kernel =="
# The check that matters after an update, and it loads nothing.
#
# It used to run `kldload -n /boot/modules/npuep.ko`, on the belief - written into the comment
# that used to be here - that kldload reports a version mismatch rather than loading. It does
# not. There is no dry run; -n means "do not load if it is already loaded", and this branch only
# runs when it is not. So the check LOADED the module. On a firewall, from a script whose own
# header promises that it changes nothing. And when that load succeeded, every check after it
# passed - so a boot that had failed was reported as a healthy chain, which is the one
# circumstance this file exists to catch.
#
# It cannot be done by trying the load anyway. A module's kernel dependency is a RANGE, from the
# __FreeBSD_version it was built against to the end of that branch, so a module built for 15.1
# loads cleanly into any later 15.x kernel rather than being refused. The mismatch has to be
# caught out of band, by comparing the stamp contrib/npuep/build.sh writes beside the module.
if kldstat -q -n npuep; then
    ok "npuep is loaded"
elif [ ! -f /boot/modules/npuep.ko ]; then
    bad "npuep is NOT loaded and there is no module in /boot/modules to load"
else
    bad "npuep is NOT loaded"
    built=$(cat /boot/modules/npuep.ko.kernel 2>/dev/null)
    now=$(uname -v)
    if [ -z "${built}" ]; then
        note "no build stamp beside the module, so this cannot tell whether it matches"
        note "to see why it did not load: kldload /boot/modules/npuep.ko"
    elif [ "${built}" != "${now}" ]; then
        bad "it was built against a different kernel - rebuild: sh contrib/npuep/build.sh"
        printf '        built  : %s\n' "${built}"
        printf '        running: %s\n' "${now}"
    else
        note "it matches this kernel, so it failed to load for some other reason"
        note "to see it: kldload /boot/modules/npuep.ko"
    fi
fi

echo
echo "== the coprocessor =="
if [ "$(sysctl -n dev.npuep.0.%desc 2>/dev/null | wc -l)" -gt 0 ] 2>/dev/null || sysctl -n dev.npuep.0.rpc_channel.open >/dev/null 2>&1; then
    ok "the endpoint attached"
else
    bad "no dev.npuep.0 - the endpoint did not attach"
fi

# All-ones is what a PCIe read returns when nothing answers. It is the signature of a coprocessor
# that has stopped, and it is worth naming rather than letting every other check fail obscurely.
db=$(sysctl -n dev.npuep.0.doorbells 2>/dev/null | sed -n '3p')
case "$db" in
    *ffffffff*) bad "the endpoint reads all ones - the coprocessor has stopped. Run contrib/npuep/reload.sh; if that does not recover it, power cycle" ;;
    '')         note "could not read the doorbell block" ;;
    *)          ok "the endpoint is answering" ;;
esac

echo
echo "== the front ports =="
n=$(ifconfig -l 2>/dev/null | tr ' ' '\n' | grep -c '^npup')
if [ "$n" -eq 14 ]; then
    ok "fourteen interfaces npup1..npup14"
elif [ "$n" -gt 0 ]; then
    bad "only $n npup interfaces - expected 14"
else
    bad "no npup interfaces at all"
    # The most common cause, and the one nobody guesses.
    if dmesg | grep -q "answers HOST_MGMT_READY once per coprocessor boot"; then
        bad "the datapath refused a second bring-up - the coprocessor has to restart. sh contrib/npuep/reload.sh pulses its reset line and waits for it, which is verified to bring all fourteen back"
    fi
fi

if dmesg | grep -q "read back and confirmed"; then
    ok "the coprocessor's interface table was programmed and read back"
else
    bad "the interface table was never confirmed - received frames will be dropped before they are counted"
fi

up=$(dmesg | grep -c "nwa: 14 of 14 ports up")
if [ "$up" -gt 0 ]; then ok "all fourteen ports were commanded up"; else note "the network agent did not report 14 of 14 ports up"; fi

echo
echo "== thermal, because this board has nothing else watching =="
t=$(sysctl -n dev.cpu.0.temperature 2>/dev/null)
if [ -n "$t" ]; then
    ok "CPU temperature readable: $t"
else
    bad "no CPU temperature - load amdtemp (this is an AMD board; coretemp is the wrong driver)"
fi
grep -q '^amdtemp_load=' /boot/loader.conf.local 2>/dev/null \
    && ok "amdtemp survives a reboot" \
    || note "amdtemp is not in loader.conf.local - the reading will be gone after a reboot"

echo
echo "== summary =="
printf '  %d passed, %d failed, %d worth a look\n' "$pass" "$fail" "$warn"
if [ "$fail" -gt 0 ]; then
    echo "  This appliance does not currently have working front ports."
    exit 1
fi
echo "  The chain is intact."
exit 0

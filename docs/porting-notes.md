# Porting notes

FreeBSD-specific things that were learned the expensive way. Everything here is measured on the
appliance, not reasoned from documentation.

## Building on the appliance

It already has a full toolchain - `cc`, `ld.lld`, `make` - and no kernel sources. Put FreeBSD's
`src.txz` for the matching release into `/usr/src` and build in place:

```sh
make SYSDIR=/usr/src/sys
```

A headers-only subset is tempting and **does not work**. `bsd.kmod.mk` needs
`sys/amd64/conf/DEFAULTS` and `sys/i386/include`, neither of which has a file extension, so any
trim rule based on extensions removes them. Unpack the whole `sys` tree; it is about 320 MB and
the appliance has room.

Getting the source there is its own problem, because the appliance has no working network until
this project succeeds. In order of preference: a USB stick (`umass`, `mount -t msdosfs
/dev/da0s1`), a USB Ethernet adapter, or the serial console at about 3 KB/s.

A **USB Ethernet adapter** is worth reaching for early. Every driver is in the stock kernel -
`axge`, `ure`, `muge`, `smsc`, `cdce` and the rest - so almost anything works with no
configuration. It turns a ninety-second transfer into a fifth of a second. Note that OPNsense's
generated ruleset with no interface assigned is `block drop in log inet all`, which also
silently eats DHCP, so the interface needs assigning properly or the firewall needs disabling
for the duration.

## bus_alloc_resource does not reprogram a cleared BAR

The single most useful fact in this file.

Resetting the coprocessor clears the endpoint's BAR registers. FreeBSD keeps serving the values
it cached at boot enumeration, so `pciconf -lb` reports the old addresses while config space
reads as zeros and every access through the mapping returns `0xFFFFFFFF`. The endpoint looks
dead when it is fine.

`bus_alloc_resource` does **not** fix this - `pci_reserve_map`, the only path that writes a BAR,
is reached only when no resource-list entry exists, which is never true after boot enumeration.
What does fix it is `pci_driver_added()` calling `pci_cfg_restore()` and thence
`pci_restore_bars()`, and that runs when a driver is added to a previously unclaimed child.

Measured immediately before and after a load:

```
before:  00000004 00000000 00000004 00000000 00000000 00000000
after:   f7000004 00000000 f6000004 00000000 f5000000 00000000
```

So `kldload` works. **`loader.conf` preload would not**, and neither would a `devctl` re-attach
after a reset while the driver is loaded. A driver that touches the reset line has to be
prepared to restore config space itself, or to be reloaded.

A related trap: **do not read an endpoint's config space while its processor is in reset**. It
does not answer, everything comes back as ones and zeros, and it is easy to conclude the
registers were cleared when the device simply was not there. That conclusion cost an
unnecessary round of `pciconf -w` writes.

## The callout mutex

`callout_init_mtx()` means the callout subsystem takes the mutex before calling the body and
drops it after. Taking it again inside the callback is a recursive acquire of a non-recursive
`MTX_DEF` mutex:

- on a kernel with `INVARIANTS`, an immediate panic on the first tick;
- on a production kernel the assertion is compiled out, the lock recurses silently and the
  matching unlock rebalances it, so it appears to work.

The second is worse than the first. `mtx_assert(&sc->mtx, MA_OWNED)` at the top of the callback
documents the contract and costs nothing. Calling `callout_reset()` from inside the body with
the lock held is correct and is the documented idiom.

## Never trust an offset the device published

The control facility's offset is read out of a structure the coprocessor writes. It is then used
as a `bus_write_4` offset, once a second, forever. Unbounded, that hands a coprocessor the
ability to steer the host's writes anywhere in the mapped window.

Bound every device-supplied offset against the window actually mapped, and guard the window
arithmetic itself - `size - constant` underflows to something enormous if the BAR is ever
smaller than the constant.

## Bus mastering is not a set-and-forget

`pci_enable_busmaster()` has to happen before MSI-X can be delivered, so it cannot be left until
last. It must be tracked and cleared on **every** exit path, including every `goto fail` in
attach.

An endpoint left bus-mastering with no driver behind it, having been told a host driver is
ready, is a device writing into host memory nobody arranged. The symptom is a general protection
fault in an unrelated subsystem - a non-canonical pointer in a kernel structure that has nothing
to do with the driver - some minutes later, with no device errors logged in between. There is
nothing in the dump that names the cause; the only reason it was attributable at all is that the
conditions had been created by hand minutes earlier.

## A heartbeat counter proves nothing

A counter of how many times the host wrote the heartbeat says only that the host kept writing.
A dead endpoint does not disturb it.

Export the value actually **read back** each tick instead. On this hardware that immediately
exposed a refactoring mistake - an offset that had lost its `+ 4` and was reading the facility
cookie rather than the handshake word, writing it back over itself unchanged. It had been
running for several minutes and would not have been noticed otherwise, because every other
indicator looked healthy.

## if_init is not optional

`if_alloc()` zeroes the ifnet. If a driver never calls `if_setinitfn()`, `ifp->if_init` stays
NULL - and the stack calls it through a bare function pointer without checking.

`in6_update_ifa()` does exactly that when an address is added to an interface that is up but not
yet `IFF_DRV_RUNNING`. So the first `ifconfig mvmgmt0 inet6 ...` against a driver missing that one
line is a call to address zero:

```
--- trap 0xc, rip = 0, rsp = 0xfffffe008b5f57d8 ---
??() at 0/frame 0xfffffe008b5f5810
in6_update_ifa() at in6_update_ifa+0x81f
in6_ifattach() at in6_ifattach+0x7f5
in6_if_up() at in6_if_up+0x7f
nd6_ioctl() at nd6_ioctl+0x7ba
ifioctl() at ifioctl+0x833
current process = ifconfig,  fault virtual address = 0x0
fault code = supervisor read instruction, page not present
```

Two things make this expensive to diagnose. The backtrace names only the network stack, so the
driver that caused it is not in it. And `fault virtual address = 0x0` reads like a null *data*
pointer, which sends you looking at buffers and DMA; the words that matter are **read
instruction** and `rip = 0`, which say the CPU jumped to zero rather than dereferenced it.

Worth stating plainly, because it happened here: this panic was first blamed on an unrelated
ring-index hazard found by reading the code, and that guess was written up and published before
the dump was read. The dump refuted it in one line. **Read the dump.**

## Reading the coprocessor's console

See [npu-bring-up.md](npu-bring-up.md). Two rules, both learned by breaking them:

- Set `-echo raw` on the line, or the host echoes the coprocessor's own output back into its
  receiver and derails its bootloader.
- Never open it with a blocking read from a session running on the host's own console. That
  hangs the machine.

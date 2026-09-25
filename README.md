# os-npuctl

Bring the Marvell network processor on a Sophos XGS appliance out of reset under OPNsense, and
complete the handshake it waits for.

These appliances are an x86 host with a Marvell CN913x coprocessor behind a PCIe endpoint, and
**the coprocessor owns every front port**. Install a stock OPNsense on one and it boots to a
working firewall with no network interfaces at all. The vendor's drivers are Linux-only, so the
usual answer is that the hardware is unusable.

It is not. The coprocessor is a whole computer that boots its own Linux from its own eMMC, and
it is sitting there waiting to be told a host is present.

## What this does today

- **Releases the coprocessor from reset** at every boot. It comes out of power-on held, and
  nothing in OPNsense releases it, which is why its console is silent and its endpoint answers
  nothing.
- **Completes the facility handshake** from a kernel module: maps the BARs, reads the map the
  coprocessor publishes, allocates the five MSI-X doorbells it asks for, sets the two bits it is
  blocked on, and holds the heartbeat.
- **Tools** to inspect all of it from userspace without changing anything.

At that point the coprocessor stops waiting and starts its own dataplane.

## What this does not do yet

**It moves packets on the management link, and not on the front ports.** `mvmgmt0` carries
traffic between the host and the coprocessor over two rings in host memory - verified on the
hardware, zero loss in both directions - and it is the interface every Sophos diagnostic tool on
the appliance talks over. It is specified in [docs/mvmgmt.md](docs/mvmgmt.md) and implemented in
`contrib/npuep/npumgmt.c`.

The fourteen front ports belong to a second, much larger facility: GIU, a full NIC with a command
channel, traffic classes, buffer pools and offloads. It is specified in
[docs/giu.md](docs/giu.md) and **not implemented**. How the fourteen ports are told apart on one
trunk is answered there: a two-byte port identifier in front of every frame. What is not
published, and would have to be recovered from a binary, is the message set that reads and sets
each port's link state, speed and MTU.

[DESIGN.md](DESIGN.md) says where the line currently is. If you are looking for working front
ports today, this is not that yet. It is the part underneath them, and it is the part that had to
exist first.

## Hardware

Written and measured on a **Sophos XGS 136** (assembly AMDA0201, CN9131, 14 ports) running
OPNsense 26.7 on FreeBSD 15.1.

The per-board reset values are a table, not a constant - the polarity is inverted between board
generations - so the module reads the assembly number out of the bridge's own EEPROM and looks
it up. Values are carried for AMDA0200, AMDA0201 (XGS 126/136), AMDA0202-0205, AMDA0208
(XGS 116) and AMDA0224 (XGS 138). Only AMDA0201 has been tested on real hardware. The others
come from the vendor tool and should be treated as unverified.

## Installing

```sh
git clone https://github.com/AbdelmonemAwad/os-npuctl
cd os-npuctl
./install/install.sh
```

The kernel module is **not** built or loaded by the installer. It needs kernel sources on the
appliance and it is not something to load unattended - see
[docs/porting-notes.md](docs/porting-notes.md).

What the installer does put in place is the reset hook, which is the half that is safe to run
at every boot and does nothing at all on hardware it does not recognise.

## Licence

BSD-2-Clause, except that the protocol it speaks was recovered from Marvell's GPL-2.0-only
`pcie_ep_armada` driver, published by Sophos in its SFOS_OSS source ISO. No vendor code is
included or redistributed here; `docs/` records what the protocol is, and the implementation is
original. If you intend to reuse this, read
[docs/facility-protocol.md](docs/facility-protocol.md) first and form your own view.

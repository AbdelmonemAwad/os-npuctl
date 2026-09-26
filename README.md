# os-npuctl

Make the fourteen front ports of a Sophos XGS appliance work under OPNsense.

These appliances are an x86 host with a Marvell CN913x coprocessor behind a PCIe endpoint, and
**the coprocessor owns every front port**. Install a stock OPNsense on one and it boots to a
working firewall with no network interfaces at all. The vendor's drivers are Linux-only, so the
usual answer is that the hardware is e-waste.

It is not. The coprocessor is a whole computer that boots its own Linux from its own eMMC, and it
is sitting there waiting to be told a host is present.

## What works

**All fourteen front ports carry traffic, in both directions, as fourteen ordinary FreeBSD
interfaces.**

```
npuep0: giu: datapath enabled - 14 interfaces, 256 descriptors each way
npuep0:   Port1   npup1 tag 0x8100      Port2   npup2 tag 0x8200
npuep0:   Port7   npup7 tag 0x8700      Port8   npup8 tag 0x8800
npuep0:   Port9   npup9 tag 0x0001      Port10  npup10 tag 0x0003
npuep0:   PortF1  npup13 tag 0x8900     PortF2  npup14 tag 0x8a00
npuep0: rpc: all 14 front ports have an interface and a binding, read back and confirmed
npuep0: nwa: 14 of 14 ports up
```

Measured with three cables in, frames arriving on three ports at once and each landing on its own
interface:

```
dev.npuep.0.giu.rx_port.Port9: 34      dev.npuep.0.giu.rx_port.Port7: 27
dev.npuep.0.giu.rx_port.Port8: 10
```

Port 8 is behind the coprocessor's internal switch and Port 9 is a separate MAC on the SoC, so
both port families are proven together. A full ARP exchange completes over either — request in,
reply out — which is the smallest thing that requires both directions to work.

The driver loads itself at boot, creates the interfaces, programs the coprocessor and verifies the
programming by reading it back. Nothing is typed.

## How it works, briefly

The coprocessor publishes a map of five *facilities* in a PCIe BAR, each one a different
conversation:

| Facility | What it is | Where |
|---|---|---|
| `ctrl` | the handshake and the doorbells | [docs/facility-protocol.md](docs/facility-protocol.md) |
| `mvmgmt` | a management NIC between host and coprocessor | [docs/mvmgmt.md](docs/mvmgmt.md) |
| `giu` | the datapath: a full NIC with queues, buffer pools and offloads | [docs/giu.md](docs/giu.md) |
| `nwa` | a mailbox for per-port state — link, speed, media, admin up | [docs/netagent.md](docs/netagent.md) |
| `rpc` | the control channel that fills in the forwarding tables | [docs/rpc.md](docs/rpc.md) |

The datapath carries every front port on **one** pair of DMA queues. What separates them is a
two-byte tag in front of each frame: the driver writes it on transmit to choose the egress port,
and reads it on receive to decide which interface a frame belongs to. Ports come in two families
and they do not follow one rule — ten behind an internal switch carry `0x8000 + n*0x100`, four
that are separate MACs on the SoC carry `0x0001`..`0x0004`.

Receive needs the coprocessor's own forwarding tables filled in, which is what the control channel
is for: a logical interface per port, and a binding from the port tag to it. Two commands each,
and nothing else.

## What it cannot do

**The datapath attaches once per coprocessor boot.** The device waits for `HOST_MGMT_READY`
once, answers once, and then spends the rest of its life in its command loop. **A module reload on
its own cannot be answered** — three remedies were tried and measured not to help: closing the
datapath down with `PF_DISABLE`/`PF_CLOSE`, which the device accepts and which changes nothing;
retracting the stale handshake; and waiting thirty seconds instead of four.

What restores it is a coprocessor reboot. A cold power cycle gives one, and that is verified — the
boot hook brings everything back with nothing typed. The host can also cause one itself:
`01-npuctl` pulses the reset line, and that pulse is a real reset rather than a release. Since it
runs at every boot, a warm reboot probably restores the ports too — **but that has not been
tested**, and this file previously claimed the opposite without evidence.

What *is* tested is the hazard: loading the driver too soon after a reset pulse **hangs the host**.
A PCIe read to an endpoint still in reset neither returns nor times out, so there is no panic and
no log — the machine simply stops. Nothing here checks that the endpoint is alive before its first
read, which makes this a risk at every boot and not only in an experiment.

**The interfaces are not assigned in OPNsense yet.** They exist, they carry traffic, and they can
be bridged — but until they are assigned they are outside the firewall's own configuration and pf
has no rules for them.

**The device's own packet counters are unavailable.** `GET_STATISTICS` is answered, at full
length, with zeros: Marvell's header labels that member `CC_PF_PP2_STATISTICS`, the physical
packet processor's counters, and a function with no physical port has none. `GET_GP_STATS`, which
is the GIU port's own, is not implemented by this firmware at all. The driver reports its own
counts and says so plainly, because an instrument that prints a zero reading like a measurement is
worse than one that admits it cannot see.

## Hardware

Written and measured on a **Sophos XGS 136** (assembly AMDA0201, CN9131, 14 ports) running
OPNsense 26.7 on FreeBSD 15.1.

The per-board reset values are a table, not a constant — the polarity is inverted between board
generations — so the module reads the assembly number out of the bridge's own EEPROM and looks it
up. Values are carried for AMDA0200, AMDA0201 (XGS 126/136), AMDA0202-0205, AMDA0208 (XGS 116)
and AMDA0224 (XGS 138). **Only AMDA0201 has been tested on real hardware.** The others come from
the vendor tool and should be treated as unverified.

## Installing

```sh
git clone https://github.com/AbdelmonemAwad/os-npuctl
cd os-npuctl
./install/install.sh
```

The installer puts in place two early boot hooks and, if a module has been built, installs it:

- `01-npuctl` pulses the coprocessor out of reset. It comes out of power-on **held**, and nothing
  in OPNsense releases it — which is why the appliance boots with a silent coprocessor and no
  ports.
- `02-npuep` loads the driver and waits for the interfaces to appear, because every early hook
  runs before OPNsense configures its interfaces and one that returns too soon leaves them out of
  that pass.

The kernel module is **built on the appliance**, not packaged: it is C against that kernel's
headers. `contrib/npuep/build.sh` builds it; the installer then copies it to `/boot/modules`.

It cannot be preloaded from `loader.conf`, and not for one reason but two — the reset pulse has
not happened at that point, so there is no live device to attach to, and the BAR restore this
hardware needs only happens on the `kldload` path. See
[docs/porting-notes.md](docs/porting-notes.md).

Both hooks are written so that they can never be the reason a firewall fails to boot: unfamiliar
hardware, a missing module or a coprocessor that never answers each log the reason and exit 0.

## Documentation

[DESIGN.md](DESIGN.md) is the contract. The `docs/` directory is the protocol work — what each
facility is, how it was read, and which claims are measured rather than inferred.

Much of it was recovered from Sophos's own shipped binaries, which carry full debug information,
and from Marvell's GPL source drop. Where a claim comes from a disassembly it says so; where it
comes from the vendor's own boot log it quotes the line.

## Licence

BSD-2-Clause, except that the protocol it speaks was recovered from Marvell's GPL-2.0-only
`pcie_ep_armada` driver, published by Sophos in its SFOS_OSS source ISO. No vendor code is
included or redistributed here; `docs/` records what the protocol is, and the implementation is
original. If you intend to reuse this, read
[docs/facility-protocol.md](docs/facility-protocol.md) first and form your own view.

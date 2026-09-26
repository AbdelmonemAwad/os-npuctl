# os-xgs-npu

**All fourteen front ports of a Sophos XGS 136, working under OPNsense.**

[![checks](https://github.com/AbdelmonemAwad/os-xgs-npu/actions/workflows/checks.yml/badge.svg)](https://github.com/AbdelmonemAwad/os-xgs-npu/actions/workflows/checks.yml)
[![licence](https://img.shields.io/badge/licence-BSD--2--Clause-blue.svg)](LICENSE)
[![OPNsense](https://img.shields.io/badge/OPNsense-26.7-d94f00.svg)](https://opnsense.org/)
[![FreeBSD](https://img.shields.io/badge/FreeBSD-15.1--RELEASE--p1-ab2b28.svg)](https://www.freebsd.org/)
[![front ports](https://img.shields.io/badge/front%20ports-14%2F14-brightgreen.svg)](#-what-works)
[![tested on](https://img.shields.io/badge/tested%20on-XGS%20136%20(AMDA0201)-lightgrey.svg)](#%EF%B8%8F-hardware)

This appliance looks like one computer and is two: an x86 host, and a Marvell CN9131 coprocessor
behind a PCIe endpoint that **owns every front port**. Install a stock OPNsense on one and it
boots to a working firewall with no network interfaces at all. The vendor's drivers are
Linux-only, so the usual answer is that the hardware is e-waste.

It is not. The coprocessor is a whole computer that boots its own Linux from its own eMMC, and it
is sitting there waiting to be told a host is present.

> **Scope.** Everything here was written and measured on **one appliance**, a Sophos XGS 136
> (assembly AMDA0201, CN9131, 14 ports). Values for sibling assemblies are carried in the tree
> because they were read out of the vendor's own tables, and they are marked as untested wherever
> they appear. No other model has been on the bench.

## ✅ What works

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

**Twelve of the fourteen are verified port by port, with loopback cables.** An ARP exchange with
an outside device proves one path; it says nothing about the other eleven, and nothing at all when
the far end declines to answer. `contrib/npuep/portmap.sh` removes the far end from the question:
it transmits out of each port in turn and records which port hears it.

```
  sent on   heard on
  npup3      npup4(+1)        0x8300 → 0x8400   switch
  npup4      npup3(+1)
  npup5      npup6(+2)        0x8500 → 0x8600   switch
  npup7      npup8(+1)        0x8700 → 0x8800   switch
  npup9      npup10(+1)       0x0001 → 0x0003   SoC
  npup11     npup12(+2)       0x0004 → 0x0002   SoC
  npup13    - nothing -       SFP cage, no fibre to hand
```

Every pair symmetric, both tag families, and **the sending port's own counter never moved** — so
the coprocessor's switch does not forward between front ports behind the host's back. That last
one is not a detail: if it did, traffic would pass between two ports without `pf` ever seeing it.
The host is the only forwarder here, which is what a firewall needs.

It also settles the one part of the port table that was inferred rather than read. The four SoC
ports are tagged `0x0001, 0x0003, 0x0004, 0x0002` in connector order — not sequentially — and that
ordering came out of a disassembly. If two of those were swapped, a frame leaving `npup10` would
have arrived on `npup11` instead of `npup9`. It arrived on `npup9`.

`PortF1` and `PortF2` are the SFP cages and are **untested**: a copper patch lead cannot loop them.

The driver loads itself at boot, creates the interfaces, programs the coprocessor and verifies the
programming by reading it back. Nothing is typed.

## 🧭 How it works, briefly

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

## ⚠️ What it cannot do

**The datapath attaches once per coprocessor boot.** The device waits for `HOST_MGMT_READY`
once, answers once, and then spends the rest of its life in its command loop. **A module reload on
its own cannot be answered** — three remedies were tried and measured not to help: closing the
datapath down with `PF_DISABLE`/`PF_CLOSE`, which the device accepts and which changes nothing;
retracting the stale handshake; and waiting thirty seconds instead of four.

What restores it is a coprocessor reboot, and the host can cause one itself. `06-npuctl` pulses the
reset line, and that pulse is a real reset rather than a release — so **a pulse followed by a
reload brings everything back with no power cycle.** Verified, twice in a row: fourteen
interfaces, the forwarding tables read back and confirmed, `nwa: 14 of 14 ports up`, and a front
port brought up with no address counted forty frames off the wire in twelve seconds. A cold power
cycle works too. A warm reboot runs the same hook, so it very probably does as well — still
untested end to end, but no longer resting on an untested mechanism.

This file used to say a power cycle was the only way. That was wrong, and both reasons looked like
hardware rather than like bugs: `reload.sh` parsed the PCI selector with an `awk` field separator
that left out the tab `pciconf` prints, so it waited its whole timeout and reported a dead
endpoint that had been answering all along; and the barmap parser read an entry the coprocessor
had not written yet as a real one — every field zero, and zero is the control facility's id — so a
half-published table failed attach outright. Both fixed.

**The facility table takes about fourteen seconds to appear after a reset, and is not published
atomically.** The cookie lands before the entries do, so validating the cookie and reading on gets
a partial table. The driver now waits for the facilities it actually needs. Measured at exactly
fourteen seconds on two consecutive cycles.

Loading the driver too soon after a reset pulse **hangs the host**: a PCIe read to an endpoint
still in reset neither returns nor times out, so there is no panic and no log — the machine simply
stops. Both the driver and `reload.sh` now ask **config space** first, which is the safe question:
a configuration read to a device that is not answering comes back as all-ones instead of being
left outstanding. Measured after the fix, the endpoint answers config space the instant the pulse
ends, so the guard costs nothing and only the facility table needs waiting for.

**The interfaces are not assigned in OPNsense yet.** They exist, they carry traffic, and they can
be bridged — but until they are assigned they are outside the firewall's own configuration and pf
has no rules for them.

**The device's own packet counters are unavailable.** `GET_STATISTICS` is answered, at full
length, with zeros: Marvell's header labels that member `CC_PF_PP2_STATISTICS`, the physical
packet processor's counters, and a function with no physical port has none. `GET_GP_STATS`, which
is the GIU port's own, is not implemented by this firmware at all. The driver reports its own
counts and says so plainly, because an instrument that prints a zero reading like a measurement is
worse than one that admits it cannot see.

## 🖥️ Hardware

Written and measured on a **Sophos XGS 136** (assembly AMDA0201, CN9131, 14 ports) running
OPNsense 26.7 on FreeBSD 15.1.

The per-board reset values are a table, not a constant — the polarity is inverted between board
generations — so the module reads the assembly number out of the bridge's own EEPROM and looks it
up. Values are carried for AMDA0200, AMDA0201 (XGS 126/136), AMDA0202-0205, AMDA0208 (XGS 116)
and AMDA0224 (XGS 138). **Only AMDA0201 has been tested on real hardware.** The others come from
the vendor tool and should be treated as unverified.

## 📦 Installing

```sh
git clone https://github.com/AbdelmonemAwad/os-xgs-npu
cd os-xgs-npu
./install/install.sh
```

The installer puts in place two early boot hooks and, if a module has been built, installs it:

- `06-npuctl` pulses the coprocessor out of reset. It comes out of power-on **held**, and nothing
  in OPNsense releases it — which is why the appliance boots with a silent coprocessor and no
  ports.
- `07-npuep` loads the driver and waits for the interfaces to appear, because every early hook
  runs before OPNsense configures its interfaces and one that returns too soon leaves them out of
  that pass.

The kernel module is **built on the appliance**, not packaged: it is C against that kernel's
headers. `contrib/npuep/fetch-sources.sh` fetches the sources that match the running kernel,
pinned to the commit the kernel names in `uname -v`; `contrib/npuep/build.sh` builds against them
and stamps the result with the kernel it was built for; the installer copies both to
`/boot/modules`.

That stamp is not bookkeeping. A module's kernel dependency is a range running to the end of its
branch, so a module built for the wrong 15.x kernel **loads without complaint** rather than being
refused — the loud failure this project was designed around does not arrive. `install/verify.sh`
compares the stamp instead, and `compat.json` records `kern_version` rather than the version
labels, which do not move when OPNsense ships a kernel set inside a series.

Both hooks are numbered above OPNsense's own `05-upgrade`, which finalises a pending firmware set
and reboots from inside the early boot sequence. Numbered below it, as `01` and `02`, this code
brought the coprocessor up and then had the machine rebooted underneath it.

It cannot be preloaded from `loader.conf`, and not for one reason but two — the reset pulse has
not happened at that point, so there is no live device to attach to, and the BAR restore this
hardware needs only happens on the `kldload` path. See
[docs/porting-notes.md](docs/porting-notes.md).

Both hooks are written so that they can never be the reason a firewall fails to boot: unfamiliar
hardware, a missing module or a coprocessor that never answers each log the reason and exit 0.

## 📚 Documentation

[**DESIGN.md**](DESIGN.md) is the contract — the stages, the limits that are properties of the
hardware, and every claim that turned out to be wrong, with what replaced it.

| | |
|---|---|
| [hardware.md](docs/hardware.md) | What is actually on the board, measured |
| [npu-bring-up.md](docs/npu-bring-up.md) | Getting the coprocessor out of reset, over a USB-to-SPI bridge |
| [facility-protocol.md](docs/facility-protocol.md) | The five facilities, the barmap, the handshake |
| [mvmgmt.md](docs/mvmgmt.md) | `mvmgmt0`, the management interface |
| [giu.md](docs/giu.md) | The datapath that carries all fourteen ports, and the 66-byte header |
| [rpc.md](docs/rpc.md) | The control channel and the forwarding tables |
| [netagent.md](docs/netagent.md) | Per-port state, link, media and address |
| [porting-notes.md](docs/porting-notes.md) | What a port to another OS would hit |
| [provenance.md](docs/provenance.md) | What was read, from where, and what was deliberately not copied |

Much of it was recovered from Sophos's own shipped binaries, which carry full debug information,
and from Marvell's GPL source drop. Where a claim comes from a disassembly it says so; where it
comes from the vendor's own boot log it quotes the line. **Where something is inferred rather than
measured, it says that too** — several things in this file were once stated with more confidence
than the evidence carried, and the corrections are in the history.

## ⚖️ Licence

**BSD-2-Clause.** Every file carries the identifier; [LICENSE](LICENSE) is the whole of it.

The protocol it speaks was recovered from Marvell's GPL-2.0-only sources, published by Sophos in
its SFOS_OSS drop, and from binaries the appliance ships. **No vendor code is included or
redistributed here.** Facts — offsets, command numbers, field widths — are transcribed; expression
is not copied, and `contrib/npuep/npugiu.h` is where that decision is visible and argued.

[**docs/provenance.md**](docs/provenance.md) sets out exactly what was read, from where, which four
sentences are quoted verbatim and why, and what is deliberately absent. Read it before reusing
this, and form your own view.

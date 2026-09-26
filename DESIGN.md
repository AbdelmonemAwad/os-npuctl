# Design

## The shape of the machine

A Sophos XGS appliance looks like one computer and is two.

```
  +-----------------------------+            +--------------------------------+
  |  x86 host                   |            |  Marvell CN913x "NPU"          |
  |  AMD Ryzen Embedded R1606G  |            |  its own Linux, its own eMMC   |
  |  runs OPNsense              |            |  owns ALL 14 front ports       |
  |                             |            |                                |
  |            PCIe root -------+---- x4 ----+--- PCIe endpoint 11ab:7080     |
  |                             |   Gen3     |                                |
  |            USB -------------+------------+--- MCP2210 bridge -> reset     |
  |            uart2 0x3E8 -----+------------+--- the NPU's console           |
  |            SMBus -----------+------------+--- board VPD EEPROM            |
  +-----------------------------+            +--------------------------------+
```

The host has **no network hardware of its own**. Every port on the front panel belongs to the
coprocessor, and the only way to reach them is to get the coprocessor running and then speak to
it over PCIe.

## The stages, and where the line is now

```
 1   release the NPU from reset           DONE     src/etc/rc.syshook.d/early/01-npuctl
 2   complete the facility handshake      DONE     contrib/npuep/npuep.c
 3   the management interface, mvmgmt0    WORKING  contrib/npuep/npumgmt.c
 4a  the AGNIC command channel            WORKING  contrib/npuep/npugiu.c
 4b  traffic classes, queues, buffers     WORKING  contrib/npuep/npugiu.c
 4c  fourteen netdevs on the trunk        WORKING  contrib/npuep/npugiu.c
 4d  the 66-byte header, both directions  WORKING  contrib/npuep/npugiu.c
 4e  per-port control, the nwa mailbox    WORKING  contrib/npuep/npunwa.c
 4f  the rpc channel and the tables       WORKING  contrib/npuep/npurpc.c
 5   loading at boot                      WORKING  src/etc/rc.syshook.d/early/02-npuep
 6   assignment in OPNsense               not started
```

**The front ports carry traffic in both directions.** Measured with three cables in, frames
arriving on three ports at once and each landing on its own interface, and a full ARP exchange
completing over a switch port and over a SoC port. That is the line that moved.

Stage 4e's open question - how a request is signalled to a far side with no doorbell - is
answered: a turn register the host writes rather than a doorbell, `NWA_TURN` at offset `0x18`.

Stage 4f did not exist when this was first written. Receive needs the coprocessor's own
forwarding tables filled in, and that is a fifth facility with its own protocol - see
[docs/rpc.md](docs/rpc.md). It is where most of the difficulty turned out to be.

What is left is stage 6: the interfaces exist and carry traffic, but until they are assigned in
OPNsense they are outside the firewall's own configuration and pf has no rules for them.

### Three limits that are properties of the hardware

**The datapath attaches once per coprocessor boot.** The device waits for `HOST_MGMT_READY`
once, answers once, and then spends the rest of its life in its command loop. **A module reload on
its own cannot be answered.** Three remedies were tried and measured not to help:
`PF_DISABLE`/`PF_CLOSE`, which the device accepts and which changes nothing; retracting the stale
handshake; and waiting thirty seconds instead of four.

What restores it is a coprocessor reboot, and the host owns the means: `01-npuctl` pulses the
reset line, and the pulse is a reset rather than a release. **Verified: a pulse followed by a
reload brings back all fourteen interfaces, the forwarding tables and the network agent, with no
power cycle** - `nwa: 14 of 14 ports up`, and a front port brought up with no address counted
forty frames off the wire in twelve seconds. A cold power cycle is verified too. A warm reboot
runs that same hook, so it very probably works as well - still **untested** as a whole, though the
mechanism it would rely on no longer is.

This file used to say a power cycle was the only thing that worked. That was wrong, and it was
wrong for a reason worth keeping, because both causes looked exactly like hardware:

- `reload.sh` parsed the PCI selector with `awk -F'[@ ]'`, and `pciconf -l` separates the selector
  from the class with a **tab**. So the selector came out as `pci0:1:0:0:<tab>class=0x020000`,
  every probe failed to parse, the script waited its full ninety seconds and reported a dead
  endpoint that had been answering from the first second.
- The barmap parser read an entry the coprocessor had not written yet as a real one. All four
  fields are zero in an unwritten entry, and zero is `MV_FACILITY_CONTROL`, so a half-published
  table came back as "control facility is not on BAR2" and failed attach outright.

**The facility table is published about fourteen seconds after a reset, and not atomically.**
Measured here: the cookie reads zero for thirteen seconds, the entries then appear in order, and
the table is complete at fourteen. The cookie is not a commit - it is in place before the entries
are - so `npuep_wait_barmap` polls for the facilities it needs rather than for the cookie, up to
two minutes, and goes on with whatever is published if it runs out.

That timing is also why none of this showed at boot: `01-npuctl` pulses reset there too, but a
minute of other boot work happens before the module loads, so the table is long finished. The
defect was invisible on the only path that was ever run.

**Loading the driver while the endpoint is in reset hangs the host.** Measured: a reset pulse, a
forty-five second wait and a `kldload` stopped the machine dead - no panic, no console output,
nothing. A PCIe read to an endpoint in reset neither completes nor times out.

Guarded in two places now. Attach asks **config space** whether the device answers before it reads
any memory, which is the safe question: a configuration read to a device that is not answering is
completed by the root complex as all-ones rather than left outstanding, so it returns instead of
stopping the machine. `reload.sh` asks the same question before it loads. Measured after the fix,
the endpoint answers config space immediately after a pulse - so the guard costs nothing on the
path that works, and only the facility table needs waiting for.

**Programming the forwarding tables races the coprocessor's own startup.** Its userspace fastpath
starts in response to this host's handshake and zeroes the whole logical-interface table about
thirteen seconds later. Commands sent before that are accepted, answered `rc 0`, and erased. So
programming runs on a thread, reads back, and repeats until the read agrees.

**The device's own packet counters are unavailable.** `GET_STATISTICS` is answered at full length
with zeros - it is the physical packet processor's block, and a function with no physical port has
none. `GET_GP_STATS` is not implemented by this firmware at all. The driver reports its own counts
and says so, because a zero that reads like a measurement is worse than an admission.

### Stage 1 - reset

The NPU's reset line is not on a GPIO controller or a CPLD. It is on an **MCP2210 USB-to-SPI
bridge** that enumerates as an ordinary USB HID device, which FreeBSD attaches without any help.
`kldload hidraw` gives a device node and the rest is 64-byte reports.

The bridge powers up loading pin states from its own NVRAM, and on this board that state holds
the NPU **in** reset. Nothing in OPNsense changes it, which is the entire reason a stock install
finds no ports.

Two details matter and both were measured rather than assumed:

- **It has to be a pulse, not a level.** Writing the release values alone does nothing - the pin
  states read back correct and the NPU stays dead. Driving the pins to the hold state and then
  to the release state starts it every time. The part wants an edge.
- **The polarity is per board.** AMDA0200 is the exact inverse of AMDA0201. The module reads the
  assembly number from the bridge's EEPROM rather than assuming.

### Stage 2 - the handshake

Once running, the NPU publishes a small map into BAR2 and then blocks. Its own startup script
polls one 32-bit word and does nothing until it reads `0x0b`:

```
bit 0  TRGT_INIT        the NPU sets this
bit 1  HOST_INIT        the host sets this
bit 2  TRGT_H2T_DBELL   the NPU sets this
bit 3  HOST_ALIVE       the host sets this, repeatedly - the NPU clears it on every scan
```

So the whole gate is **two bits**. But they cannot simply be written, because setting them tells
the NPU that a host driver is present and ready - and it immediately starts raising doorbells,
which are MSI-X interrupts, into vectors that only a kernel can allocate. It also begins using
the host memory it has mapped.

That ordering is the single most important thing in this repository:

```
  allocate the five MSI-X vectors
  install their handlers
  THEN set HOST_INIT
  THEN set HOST_ALIVE, and keep setting it
```

Doing it the other way round - completing the handshake with nothing behind it - is a
coprocessor with bus mastering enabled, told a driver is ready, writing into host memory that
nobody vetted. It produces a general protection fault in an unrelated kernel subsystem some
minutes later, with nothing in any log to connect the two.

### Stage 3 - mvmgmt0, the management interface

A virtual Ethernet link between host and coprocessor. No wire: two rings in **host** memory,
which the NPU reaches through its inbound window using physical addresses the host publishes
into a shared structure at BAR2 + 0x1000.

This is the first point at which the host hands the coprocessor addresses in its own RAM. Up to
stage 2 everything was the host reading and writing the endpoint's BARs, where the worst case is
a confused endpoint.

The specification it is being written against is [docs/mvmgmt.md](docs/mvmgmt.md), which also
records three places where a straight transcription of the vendor driver would be wrong.

It is the right next step and not the datapath, for three reasons: it is one facility rather
than the whole GIU machinery, it is the smallest thing that can carry a packet and therefore
prove the model, and it is the link the vendor's own diagnostic tools use - on the appliance,
`xgs-cpld`, `xgs-ports`, `xgs-sff` and the rest are thin wrappers that ssh across it.

### Stage 4 - the datapath

Not a ring pair like stage 3. GIU is a whole NIC: thirty management commands over a 64-byte
descriptor channel, up to eight traffic classes each with its own queues, a buffer pool per
receive queue, checksum offload in both directions, VLAN filtering, and a per-queue MSI-X vector.

The split is also inverted. In stage 3 both rings and both index pairs live in host memory. Here
the configuration structure and **every ring's producer and consumer index live in the device's
BAR0**, while the descriptors and buffers live in host memory - so the indices are MMIO accesses,
not loads and stores.

Nothing here implements it. [docs/giu.md](docs/giu.md) is the specification, read out of the
vendor's GPL `giu_nic` source, including three defects the vendor fixed after publishing it.

**How the fourteen ports are told apart is settled**: a two-byte port identifier prepended to
every frame in network order, in front of the Ethernet header. Not the descriptor's `port_num`
field, which the vendor's host driver never reads, and not a VLAN tag - an earlier reading of the
harvested port map blamed the VLAN 4095 subinterface, and that was wrong, because 4095 is the same
on all fourteen and so cannot distinguish them.

That splits stage 4 into two pieces that are worth doing in order. **Fourteen interfaces that
carry traffic** need the GIU trunk and the two-byte tag, both fully specified. **Fourteen
interfaces whose link state, speed and MTU can be read and set** need Sophos's NetAgent message
set, which rides the AGNIC custom channel and is not published - it has to be recovered from
`mv_nwa_host` the way the MCP2210 command map was recovered from `xgs-usb-spi-flash`.

## Why the module is not loaded automatically

It is installed but never loaded by the plugin, and that is deliberate on three counts.

**A module that panics at boot gives you a machine that panics at boot.** During development
that costs a power cycle each time, and on a firewall it costs the firewall.

**`loader.conf` preload would not work anyway.** Resetting the NPU clears the endpoint's BAR
registers, and FreeBSD keeps serving its boot-time cached values, so every read comes back as
`0xFFFFFFFF`. What repairs it is `pci_driver_added()` calling `pci_cfg_restore()`, which happens
on the `kldload` path and not on preload. This was measured, not reasoned: BARs read as all
zeros immediately before the load and correct immediately after.

**The reset hook and the module have different risk profiles.** The hook touches a USB bridge,
does nothing on unrecognised hardware, and cannot hurt the host. The module allocates interrupt
vectors and invites a coprocessor to use host memory. Only the first belongs in an unattended
boot path.

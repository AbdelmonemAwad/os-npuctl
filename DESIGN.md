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

## Four stages, and where the line is

```
 1  release the NPU from reset            DONE   src/etc/rc.syshook.d/early/01-npuctl
 2  complete the facility handshake       DONE   contrib/npuep/npuep.c
 3  the management interface, mvmgmt0     WIP    contrib/npuep/npumgmt.c, not in the build
 4  the GIU datapath and the 14 ports     NOT STARTED
```

Stage 1 and 2 are the ones in this repository as working code. Stage 3 has its memory layer
written and verified against the vendor source; the ring protocol is not finished. Stage 4 is
weeks of work and nothing has been written for it.

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

One netdev carrying all fourteen ports over a GIU/AGNIC ring pair, with the ports separated by
VLAN 4095 tags on a single trunk. Sixteen management opcodes, 256-entry command and notification
rings of 64-byte descriptors, and a DMA path that has to be right.

Nothing here implements it. The protocol is documented in
[docs/facility-protocol.md](docs/facility-protocol.md) as far as it has been read.

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

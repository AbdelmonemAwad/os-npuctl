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
 1   release the NPU from reset           DONE     src/etc/rc.syshook.d/early/06-npuctl
 2   complete the facility handshake      DONE     contrib/npuep/npuep.c
 3   the management interface, mvmgmt0    WORKING  contrib/npuep/npumgmt.c
 4a  the AGNIC command channel            WORKING  contrib/npuep/npugiu.c
 4b  traffic classes, queues, buffers     WORKING  contrib/npuep/npugiu.c
 4c  fourteen netdevs on the trunk        WORKING  contrib/npuep/npugiu.c
 4d  the 66-byte header, both directions  WORKING  contrib/npuep/npugiu.c
 4e  per-port control, the nwa mailbox    WORKING  contrib/npuep/npunwa.c
 4f  the rpc channel and the tables       WORKING  contrib/npuep/npurpc.c
 5   loading at boot                      WORKING  src/etc/rc.syshook.d/early/07-npuep
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

What restores it is a coprocessor reboot, and the host owns the means: `06-npuctl` pulses the
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

That timing is also why none of this showed at boot: `06-npuctl` pulses reset there too, but a
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

#### Proving the tag table, one port at a time

An ARP exchange with a device on the far end proves one path. It proves nothing about the other
eleven, and it proves nothing either way when the far end declines to answer - which cost half an
hour here, pinging a gateway that had been unplugged from that wire hours earlier.

`tcpdump` on the sending interface does not help. That is a BPF tap taken before the frame is
handed to the coprocessor, so it says the driver transmitted. It cannot say anything came out of
the connector.

`contrib/npuep/portmap.sh` settles it without a far end at all: transmit out of every port in
turn, with a loopback cable between pairs, and record which port hears it. Twelve of the fourteen,
in one sweep:

```
  npup3 ↔ npup4     0x8300 ↔ 0x8400   switch
  npup5 ↔ npup6     0x8500 ↔ 0x8600   switch
  npup7 ↔ npup8     0x8700 ↔ 0x8800   switch
  npup9 ↔ npup10    0x0001 ↔ 0x0003   SoC
  npup11 ↔ npup12   0x0004 ↔ 0x0002   SoC
```

with `npup1 ↔ npup2` measured separately first. Three results come out of it.

**Egress reaches the connector.** Nothing before this had shown that; it was inferred from the
frame format and from one ARP exchange.

**The SoC tag order is right.** Those four are tagged `0x0001, 0x0003, 0x0004, 0x0002` in
connector order rather than sequentially, and that ordering was read out of a disassembly, never
documented. If `0x0002` and `0x0003` were the wrong way round, a frame leaving `npup10` would have
come out at the connector next to `npup11` and been heard there. It was heard on `npup9`.

**The internal switch does not forward between front ports.** The sending port's own counter never
moved, in any of the twelve. So every frame crossing between two front ports goes up to the host
and back down, and `pf` sees all of it. Had the switch forwarded on its own, rules would be
bypassed by traffic the firewall never saw - which is the kind of thing that is discovered after
it matters rather than before.

`PortF1` and `PortF2` are the SFP cages and remain **untested**: a copper patch lead cannot loop a
fibre cage, and no module was to hand.

#### Link state, and the one line that hid it

The network agent reported carrier for the four SoC ports and never once for any of the ten switch
ports - not while a switch port was linked and passing frames, and not when a cable was plugged
into one. `nwa: 14 of 14 ports up` at attach showed the agent was addressing all fourteen for the
bring-up command, so it was specific to link state.

It was arithmetic, in `npunwa_command`. The reply length counts **bytes** and includes an
eight-byte header, so a one-byte answer is nine. Dividing the payload by four and truncating gave
**zero words**: nothing was read and the caller was handed a zero, which is indistinguishable from
a definite answer of "no carrier".

The two families are answered by different code on the far side, and that is why only one of them
worked. The ten switch ports go through UMSD, whose `npu_port_state_get` sets
`ret_data.size = sizeof(param.state)` on a `u8` - nine bytes, zero words, always down. The four
SoC ports are answered by NetAgent itself with a four-byte state - twelve bytes, one word, fine.
Port9 reporting carrier while Port1 never did was not a property of the hardware.

The vendor's own host rounds up: `NWA_NUM_DATA_CHUNCK(len)` is `NWA_PCI_ALIGN(len) / 4`.

Rounding up then requires masking. The last word read may contain bytes the far side never wrote,
the window belongs to another processor, and the link poll tests the whole word as `(v != 0)` - so
a port would have read as up on the strength of somebody else's leftovers. The mask is applied in
`npunwa_command`, which fixes every caller at once and needs no per-attribute knowledge of field
widths.

#### Knowing is not telling

Reading the carrier correctly only put it in the log. Nothing told the network stack.

`npugiu_init_locked` had been declaring every port `LINK_STATE_UP` the moment the datapath came
up, which is how all fourteen showed a green plug in OPNsense's interface list from the moment the
driver loaded, including the ten with nothing plugged into them. That facility moves frames; it
has no idea whether a cable is in the socket. The claim is gone, the state now starts
`LINK_STATE_UNKNOWN`, and `npugiu_link_change` is the seam the agent calls when it learns
something. Verified with a temporary printf: indices 0, 2, 3 and 8 - exactly the four cabled ports
- reached `if_link_state` 2, and the ten empty ones did not.

This is not cosmetic. OPNsense drives gateway monitoring, failover and its `rc.linkup` hooks off
link state, and a port that is always up is a port those mechanisms are blind to.

#### Still open: there is no media layer

`ifconfig` prints its `status:` line from `SIOCGIFMEDIA`, and this driver answers no media ioctl at
all - so there is no `status: active` or `status: no carrier` on any of the fourteen, and no speed
shown either. That is a separate gap from link state and it wants `ifmedia(9)`.

Two hours went into chasing the wrong indicator first. `ifconfig` shows `0x1000000` on the USB
adapters, and it is tempting to read that as a link flag; it is `IFF_NETLINK_1`, "used by netlink".
**FreeBSD has no `IFF_LOWER_UP`** - that is a Linux flag - and link state is not visible in the
flags word at all.

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

## Where the hooks sit in the boot sequence

The hooks are `06-npuctl` and `07-npuep`, and both numbers are chosen rather than inherited.
They were `01` and `02` until an audit of the update path asked what runs between them and
OPNsense configuring its interfaces. The answer is OPNsense's own `05-upgrade`, which is this,
in full:

```sh
for STAGE in K B P; do
	if opnsense-update -${STAGE}; then echo "Rebooting now."; reboot; fi
done
```

It finalises a pending firmware set and reboots from inside the early sequence. Numbered ahead of
it, this project brought the coprocessor up, loaded the driver and let the endpoint start bus
mastering - and then that hook rebooted the machine underneath it, unattended, with nothing given
the chance to unload. Numbered after it the interaction does not exist, and it costs nothing on an
ordinary boot, because `opnsense-update` finds nothing pending and returns.

## Shutdown is not detach

`device_shutdown` was missing for the whole life of this driver, and nothing revealed it.

FreeBSD calls `device_detach` on `kldunload`. It does not call it on `reboot` - it calls
`device_shutdown`, and a driver that declares none is simply skipped. So every reboot left the
endpoint bus mastering, with its MSI-X vectors armed and the ring addresses this kernel had
published still live, writing received frames and doorbell messages into physical memory the next
kernel was about to hand to something else.

`docs/porting-notes.md` already described that symptom class under a different trigger: a fault in
an unrelated subsystem, some minutes later, with no device errors logged in between. The teardown
that prevents it had been written, carefully, and left reachable only from a path that a reboot
never takes.

### And the obvious fix hung the machine

The first attempt made the quiescing half of detach a function of its own and had both
`device_detach` and `device_shutdown` call it: withdraw the facilities newest first, drain the
heartbeat, clear `HOST_INIT` and `HOST_ALIVE`, disable bus mastering. It reads correctly. The first
reboot after it went in never came back.

```
06:16:43  reboot: rebooted by root
06:16:43  syslog-ng: syslog-ng shutting down
          ... thirty minutes of nothing ...
06:47:00  kernel: ---<<BOOT>>---        <- and kern.boottime is the power cycle
```

No `---<<BOOT>>---` in between, and the boot time afterwards is when the power was pulled. The
machine entered shutdown and never reached the reset.

**Every one of those withdrawals waits.** They wait for a coprocessor to acknowledge, and they
drain taskqueue threads. That is correct in `kldunload`, where the system is running underneath
them. Device shutdown methods run late in `kern_reboot`, after the filesystems have been flushed,
and waiting on anything there is a request to be hung.

So `device_shutdown` now does only what actually stops the endpoint writing into host memory, and
only in register writes that return:

- clear `HOST_INIT` and `HOST_ALIVE`, so the far side is told;
- clear the **bus master** bit, so it is stopped whether or not it was listening. Every DMA write
  and every MSI-X message is a memory write from the endpoint, so this covers the interrupts too -
  and it is enforced by the root complex rather than by the coprocessor's cooperation, which is why
  it is the one step that matters.

`callout_stop` rather than `callout_drain`, because stop does not wait for a callout already
running and the heartbeat's whole job is one register write that is harmless at that point. The
facility teardown stays in `device_detach`, where there is a system to wait on.

The general lesson is not about this driver. A teardown written for module unload is not a shutdown
handler, however similar the two look, and the difference does not show up in review - it shows up
as a machine that goes quiet and never comes back.

## Which kernel sources the module is built against

OPNsense ships no kernel sources and no package provides them, so this appliance had a 333MB
`/usr/src/sys` that somebody had copied there once. It built, so nobody asked what it was.

It was the wrong tree. It was stock FreeBSD 15.1-RELEASE, `BRANCH="RELEASE"`, while the kernel is
OPNsense's own build of 15.1-RELEASE-p1 from `github.com/opnsense/src`. The two differ in 156
files.

That the module worked anyway was luck, and the only way to know it was luck rather than
correctness was to fetch the right tree and rebuild against it. `__FreeBSD_version` is `1501000` in
both, none of the 156 differing files is in a path this driver includes, and **the two builds came
out byte-identical**. A good outcome, and not a reason to go on guessing - the next kernel is under
no obligation to be as kind.

The kernel names its own commit, so the sources can be pinned to exactly what is running with no
version file to keep in step:

```
FreeBSD 15.1-RELEASE-p1 stable/26.7-n283674-12334a596709 SMP
                                             ^^^^^^^^^^^^ the commit in opnsense/src
```

`contrib/npuep/fetch-sources.sh` does that, and prints the `SYSDIR` to build with. Run it *after*
the reboot that brings a new kernel up, not before: `uname -v` reports the running kernel, so
beforehand it pins the old one perfectly and uselessly.

## A mismatch does not announce itself

The assumption underneath `compat.json`, `upstream.yml` and `verify.sh` was that a module built
for the wrong kernel would be refused at load time, loudly, with a message naming the cause.

It would not. A module's kernel dependency is a **range** - from the `__FreeBSD_version` it was
compiled against to the end of that branch - so a module built for 15.1 loads cleanly into any
later 15.x kernel. Nothing is printed, nothing fails, and any structure that moved is read at the
wrong offset.

So the mismatch is caught out of band instead. `contrib/npuep/build.sh` writes the kernel it built
against into `npuep.ko.kernel`, the installer carries that stamp to `/boot/modules` beside the
module, and `verify.sh` compares strings. `compat.json` records `kern_version` for the same reason:
OPNsense ships kernel sets *within* a series, so 26.7 -> 26.7.4 replaces `/boot/kernel` while
leaving both "OPNsense 26.7" and "FreeBSD 15.1-RELEASE-p1" untouched. The labels do not move. The
commit does.
